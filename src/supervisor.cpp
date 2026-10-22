#include "supervisor.h"

#include <algorithm>

#include "util.h"

namespace uvscope {
namespace {
constexpr std::size_t kMaxEvents = 500;
}

Supervisor::Supervisor(Dispatch dispatch) : dispatch_(std::move(dispatch)) {
  thread_ = std::thread([this] { loop(); });
}

Supervisor::~Supervisor() {
  stop();
}

void Supervisor::stop() {
  if (stop_.exchange(true)) return;
  wait_cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void Supervisor::add_event(const std::string& type, const std::string& id,
                           const std::string& name, const std::string& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  nlohmann::json event = {{"seq", next_event_++}, {"at", now_iso8601()},
                          {"type", type}, {"id", id}, {"name", name}};
  if (!message.empty()) event["message"] = message;
  events_.push_back(std::move(event));
  if (events_.size() > kMaxEvents) {
    events_.erase(events_.begin(), events_.begin() + (events_.size() - kMaxEvents));
  }
}

void Supervisor::register_service(const std::string& kind, const std::string& id,
                                  const std::string& name, bool desired,
                                  std::function<bool(std::string*)> start,
                                  std::function<bool(std::string*)> stop,
                                  std::function<bool()> running) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    Service& service = services_[id];
    service.id = id;
    service.kind = kind;
    service.name = name;
    service.desired = desired;
    service.start = std::move(start);
    service.stop = std::move(stop);
    service.running = std::move(running);
  }
  add_event("registered", id, name, kind + (desired ? " (desired)" : ""));
}

void Supervisor::unregister_service(const std::string& id) {
  std::string name;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = services_.find(id);
    if (it == services_.end()) return;
    name = it->second.name;
    services_.erase(it);
  }
  add_event("unregistered", id, name);
}

void Supervisor::set_desired(const std::string& id, bool desired) {
  std::string name;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = services_.find(id);
    if (it == services_.end()) return;
    it->second.desired = desired;
    if (desired) it->second.next_attempt = {};
    name = it->second.name;
  }
  add_event(desired ? "enabled" : "disabled", id, name);
}

bool Supervisor::restart(const std::string& id) {
  std::function<bool(std::string*)> start;
  std::function<bool(std::string*)> stop;
  std::string name;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = services_.find(id);
    if (it == services_.end()) return false;
    start = it->second.start;
    stop = it->second.stop;
    name = it->second.name;
    it->second.desired = true;
    it->second.next_attempt = {};
  }
  add_event("restart-requested", id, name);
  dispatch_([this, id, name, start, stop] {
    std::string error;
    if (stop) stop(&error);
    if (!start || !start(&error)) {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto it = services_.find(id);
      if (it != services_.end()) it->second.last_error = error;
    }
  });
  return true;
}

bool Supervisor::stop_runtime(const std::string& id) {
  std::function<bool(std::string*)> stop;
  std::string name;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = services_.find(id);
    if (it == services_.end()) return false;
    stop = it->second.stop;
    name = it->second.name;
  }
  add_event("fail-runtime", id, name);
  dispatch_([this, id, name, stop] {
    std::string error;
    if (stop) stop(&error);
  });
  return true;
}

void Supervisor::loop() {
  std::unique_lock<std::mutex> wait_lock(wait_mutex_);
  while (!stop_.load()) {
    wait_cv_.wait_for(wait_lock, std::chrono::seconds(2),
                      [this] { return stop_.load(); });
    if (stop_.load()) break;
    wait_lock.unlock();

    struct Attempt {
      std::string id;
      std::string name;
      std::function<bool(std::string*)> start;
    };
    std::vector<Attempt> attempts;
    const auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto& item : services_) {
        Service& service = item.second;
        const bool running = service.running ? service.running() : false;
        if (!service.desired) continue;
        if (running) {
          if (service.pending_restart) {
            service.pending_restart = false;
            ++service.restarts;
            service.updated_at = now_iso8601();
          }
          service.next_attempt = {};
          continue;
        }
        if (now < service.next_attempt) continue;
        const int backoff_seconds =
            std::min(60, 2 << std::min(service.restarts, 4));
        service.next_attempt = now + std::chrono::seconds(backoff_seconds);
        service.pending_restart = true;
        service.last_error.clear();
        attempts.push_back({service.id, service.name, service.start});
      }
    }
    for (const auto& attempt : attempts) {
      add_event("restart", attempt.id, attempt.name,
                "service was not running; restarting");
      dispatch_([this, attempt] {
        std::string error;
        if (!attempt.start || !attempt.start(&error)) {
          std::lock_guard<std::mutex> lock(mutex_);
          const auto it = services_.find(attempt.id);
          if (it != services_.end()) it->second.last_error = error;
        }
      });
    }
    wait_lock.lock();
  }
}

nlohmann::json Supervisor::snapshot() const {
  nlohmann::json services = nlohmann::json::array();
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& item : services_) {
    const Service& service = item.second;
    services.push_back({{"id", service.id},
                        {"kind", service.kind},
                        {"name", service.name},
                        {"desired", service.desired},
                        {"running", service.running ? service.running() : false},
                        {"restarts", service.restarts},
                        {"lastError", service.last_error},
                        {"updatedAt", service.updated_at}});
  }
  return {{"ok", true}, {"services", services}};
}

nlohmann::json Supervisor::events(std::uint64_t after, int limit) const {
  nlohmann::json out = nlohmann::json::array();
  std::lock_guard<std::mutex> lock(mutex_);
  if (limit <= 0 || limit > 500) limit = 200;
  std::vector<nlohmann::json> selected;
  for (const auto& event : events_) {
    if (event.value("seq", std::uint64_t{0}) <= after) continue;
    selected.push_back(event);
  }
  while (selected.size() > static_cast<std::size_t>(limit)) selected.erase(selected.begin());
  for (const auto& event : selected) out.push_back(event);
  return {{"ok", true}, {"events", out}};
}

}  // namespace uvscope
