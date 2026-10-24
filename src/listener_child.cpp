#include "listener_child.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <uv.h>
#include <nlohmann/json.hpp>

#include "ipc.h"
#include "transport_manager.h"
#include "util.h"

namespace uvscope {

nlohmann::json listener_config_to_json(const ListenerConfig& config) {
  return {{"id", config.id},
          {"transport", config.transport},
          {"name", config.name},
          {"host", config.host},
          {"port", config.port},
          {"autoStart", config.auto_start},
          {"isolated", config.isolated}};
}

bool listener_config_from_json(const nlohmann::json& value, ListenerConfig* config,
                               std::string* error) {
  if (!value.is_object() || config == nullptr) {
    if (error) *error = "listener config must be a JSON object";
    return false;
  }
  ListenerConfig parsed;
  parsed.id = value.value("id", std::string());
  parsed.transport = value.value("transport", std::string("tcp"));
  parsed.name = value.value("name", std::string());
  parsed.host = value.value("host", std::string("127.0.0.1"));
  parsed.port = value.value("port", 0);
  parsed.auto_start = value.value("autoStart", false);
  parsed.isolated = false;
  if (parsed.id.empty() ||
      (parsed.transport != "tcp" && parsed.transport != "udp") ||
      parsed.host.empty() || parsed.port <= 0 || parsed.port > 65535) {
    if (error) *error = "invalid listener config";
    return false;
  }
  *config = std::move(parsed);
  return true;
}

int run_listener_child(const std::string& endpoint) {
  uv_loop_t loop;
  if (uv_loop_init(&loop) != 0) return 1;
  TransportManager manager;
  IpcChannel channel(&loop);
  std::string listener_id;
  struct Telemetry {
    TransportManager* manager = nullptr;
    IpcChannel* channel = nullptr;
    std::string id;
    bool started = false;
    std::uint64_t last_event_seq = 0;
  } telemetry;
  telemetry.manager = &manager;
  telemetry.channel = &channel;

  channel.set_on_ready([&channel] {
    std::string error;
    channel.send({{"type", "ready"}}, &error);
  });
  channel.set_on_message(
      [&channel, &manager, &listener_id, &telemetry](const nlohmann::json& message) {
        if (!message.is_object()) return;
        const std::string type = message.value("type", std::string());
        auto reply = [&channel](const nlohmann::json& value) {
          std::string error;
          channel.send(value, &error);
        };
        if (type == "start") {
          ListenerConfig config;
          std::string error;
          if (!listener_config_from_json(
                  message.value("config", nlohmann::json::object()), &config, &error)) {
            reply({{"type", "error"}, {"error", error}});
            return;
          }
          listener_id = config.id;
          if (!manager.add_listener(config, &error) ||
              !manager.start_listener(listener_id, &error)) {
            manager.remove_listener(listener_id, nullptr);
            reply({{"type", "error"}, {"error", error}});
            return;
          }
          telemetry.id = listener_id;
          telemetry.started = true;
          telemetry.last_event_seq = 0;
          const auto current = manager.listener(listener_id);
          reply({{"type", "started"},
                 {"listener", current.value("listener", nlohmann::json::object())}});
        } else if (type == "snapshot") {
          const auto current = manager.listener(listener_id);
          reply({{"type", "snapshot"},
                 {"listener", current.value("listener", nlohmann::json::object())}});
        } else if (type == "session-events") {
          const std::string session_id = message.value("sessionId", std::string());
          const std::uint64_t after = message.value("after", 0);
          const int limit = std::max(1, std::min(message.value("limit", 200), 500));
          const auto result =
              manager.listener_session_events(listener_id, session_id, after, limit);
          reply({{"type", "result"},
                 {"ok", result.value("ok", false)},
                 {"error", result.value("error", std::string())},
                 {"events", result.value("events", nlohmann::json::array())},
                 {"session", result.value("session", nlohmann::json::object())},
                 {"nextSeq", result.value("nextSeq", 0)}});
        } else if (type == "send") {
          std::string error;
          const bool ok = manager.listener_send(
              listener_id, message.value("sessionId", std::string()),
              message.value("peerHost", std::string()), message.value("peerPort", 0),
              message.value("payload", std::string()),
              message.value("encoding", std::string("text")), &error);
          reply({{"type", "result"}, {"ok", ok}, {"error", error}});
        } else if (type == "close-session") {
          std::string error;
          const bool ok = manager.listener_close_session(
              listener_id, message.value("sessionId", std::string()), &error);
          reply({{"type", "result"}, {"ok", ok}, {"error", error}});
        } else if (type == "stop") {
          telemetry.started = false;
          if (!listener_id.empty()) {
            manager.stop_listener(listener_id, nullptr);
            manager.remove_listener(listener_id, nullptr);
          }
          reply({{"type", "stopped"}});
          channel.close();
        }
      });
  channel.set_on_close([&loop, &telemetry](const std::string&) {
    telemetry.started = false;
    uv_stop(&loop);
  });
  std::string error;
  if (!channel.connect(endpoint, &error)) {
    uv_loop_close(&loop);
    return 1;
  }
  uv_timer_t telemetry_timer;
  uv_timer_init(&loop, &telemetry_timer);
  telemetry_timer.data = &telemetry;
  uv_timer_start(
      &telemetry_timer,
      [](uv_timer_t* timer) {
        auto* state = static_cast<Telemetry*>(timer->data);
        if (state == nullptr || !state->started || state->id.empty()) return;
        const auto current = state->manager->listener(state->id);
        const auto events = state->manager->listener_events(state->id,
                                                            state->last_event_seq, 200);
        const auto sessions = state->manager->listener_sessions(state->id);
        const auto events_array = events.value("events", nlohmann::json::array());
        if (events_array.is_array() && !events_array.empty()) {
          state->last_event_seq = events_array.back().value("seq", state->last_event_seq);
        }
        std::string send_error;
        state->channel->send(
            {{"type", "telemetry"},
             {"listener", current.value("listener", nlohmann::json::object())},
             {"events", events_array},
             {"sessions", sessions.value("sessions", nlohmann::json::array())}},
            &send_error);
      },
      500, 500);
  uv_run(&loop, UV_RUN_DEFAULT);
  channel.close();
  uv_timer_stop(&telemetry_timer);
  uv_close(reinterpret_cast<uv_handle_t*>(&telemetry_timer), nullptr);
  for (int i = 0; i < 8 && uv_loop_alive(&loop); ++i) uv_run(&loop, UV_RUN_NOWAIT);
  uv_loop_close(&loop);
  return 0;
}

struct ListenerChildService::Impl {
  Impl(std::string path, ListenerConfig listener_config)
      : executable(std::move(path)), config(std::move(listener_config)) {}

  std::string executable;
  mutable std::mutex mutex;
  mutable std::condition_variable cv;
  ListenerConfig config;
  uv_loop_t loop{};
  IpcChannel* server = nullptr;
  uv_process_t process{};
  bool process_spawned = false;
  bool process_closed = false;
  mutable uv_async_t wake{};
  bool wake_initialized = false;
  mutable std::deque<nlohmann::json> commands;
  std::thread thread;
  std::atomic<bool> is_running{false};
  std::atomic<bool> stopping{false};
  bool loop_ready = false;
  bool start_result_ready = false;
  bool start_ok = false;
  std::string error;
  nlohmann::json snapshot = nlohmann::json::object();
  nlohmann::json event_cache = nlohmann::json::array();
  nlohmann::json session_cache = nlohmann::json::array();
  std::uint64_t last_event_seq = 0;
  int child_pid = 0;
  std::string endpoint;
  mutable bool request_pending = false;
  mutable nlohmann::json request_result = nlohmann::json::object();
  mutable std::string request_error;

  ListenerConfig config_copy() const {
    std::lock_guard<std::mutex> lock(mutex);
    return config;
  }

  void update_telemetry(const nlohmann::json& message) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      const auto current = message.value("listener", nlohmann::json::object());
      if (current.is_object()) snapshot = current;
      const auto sessions = message.value("sessions", nlohmann::json::array());
      if (sessions.is_array()) session_cache = sessions;
    }
    const auto events = message.value("events", nlohmann::json::array());
    if (!events.is_array()) return;
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& event : events) {
      if (!event.is_object()) continue;
      const std::uint64_t seq = event.value("seq", 0);
      if (seq == 0 || seq <= last_event_seq) continue;
      event_cache.push_back(event);
      last_event_seq = seq;
    }
    while (event_cache.size() > 500) event_cache.erase(event_cache.begin());
  }

  void on_message(const nlohmann::json& message) {
    if (!message.is_object()) return;
    const std::string type = message.value("type", std::string());
    if (type == "started") {
      {
        std::lock_guard<std::mutex> lock(mutex);
        const auto current = message.value("listener", nlohmann::json::object());
        if (current.is_object()) snapshot = current;
        is_running.store(true);
        start_ok = true;
        start_result_ready = true;
      }
      cv.notify_all();
      return;
    }
    if (type == "telemetry") {
      update_telemetry(message);
      return;
    }
    if (type == "result") {
      {
        std::lock_guard<std::mutex> lock(mutex);
        request_result = message;
        request_error = message.value("ok", false)
                            ? std::string()
                            : message.value("error", std::string("request failed"));
        request_pending = false;
      }
      cv.notify_all();
      return;
    }
    if (type == "error") {
      {
        std::lock_guard<std::mutex> lock(mutex);
        start_ok = false;
        start_result_ready = true;
        error = message.value("error", std::string("listener child error"));
        is_running.store(false);
      }
      cv.notify_all();
      uv_stop(&loop);
      return;
    }
    if (type == "stopped") {
      {
        std::lock_guard<std::mutex> lock(mutex);
        is_running.store(false);
        stopping.store(true);
      }
      cv.notify_all();
    }
  }

  void on_wake() {
    std::deque<nlohmann::json> pending;
    {
      std::lock_guard<std::mutex> lock(mutex);
      pending.swap(commands);
    }
    for (const auto& command : pending) {
      if (command.value("type", std::string()) == "shutdown") {
        uv_stop(&loop);
        continue;
      }
      if (server != nullptr) {
        std::string send_error;
        server->send(command, &send_error);
      }
    }
  }

  static void on_wake_cb(uv_async_t* handle) {
    auto* self = static_cast<Impl*>(handle->data);
    if (self != nullptr) self->on_wake();
  }

  static void on_exit_cb(uv_process_t* child, int64_t, int) {
    auto* self = static_cast<Impl*>(child->data);
    if (self == nullptr) return;
    {
      std::lock_guard<std::mutex> lock(self->mutex);
      self->process_closed = true;
      self->is_running.store(false);
      if (!self->stopping.load() && self->error.empty()) {
        self->error = "listener child exited";
      }
    }
    uv_close(reinterpret_cast<uv_handle_t*>(child), nullptr);
    uv_stop(&self->loop);
    self->cv.notify_all();
  }

  void set_start_error(const std::string& reason) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (start_result_ready) return;
      start_ok = false;
      start_result_ready = true;
      error = reason;
      is_running.store(false);
    }
    cv.notify_all();
    uv_stop(&loop);
  }

  void post(nlohmann::json message) const {
    bool signal = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      commands.push_back(std::move(message));
      signal = wake_initialized;
    }
    if (signal) uv_async_send(&wake);
  }

  nlohmann::json request(const nlohmann::json& command, int timeout_ms,
                         std::string* error) const {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (!is_running.load()) {
        if (error) *error = "listener child is not running";
        return {};
      }
      if (request_pending) {
        if (error) *error = "another listener request is in flight";
        return {};
      }
      request_pending = true;
      request_result = nlohmann::json::object();
      request_error.clear();
    }
    post(command);
    std::unique_lock<std::mutex> lock(mutex);
    if (!cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                     [this] { return !request_pending; })) {
      request_pending = false;
      if (error) *error = "listener child request timed out";
      return {};
    }
    if (!request_error.empty()) {
      if (error) *error = request_error;
      return {};
    }
    return request_result;
  }

  void loop_main() {
    if (uv_loop_init(&loop) != 0) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        start_ok = false;
        start_result_ready = true;
        error = "could not initialize the listener child IPC loop";
      }
      cv.notify_all();
      return;
    }
    endpoint = ipc_endpoint_path("listener-svc-" + random_hex(6));
    std::error_code ec;
    std::filesystem::remove(endpoint, ec);
    server = new IpcChannel(&loop);
    server->set_on_ready([this] {
      std::string send_error;
      if (!server->send(
              {{"type", "start"},
               {"config", listener_config_to_json(config_copy())}},
              &send_error)) {
        set_start_error(send_error);
      }
    });
    server->set_on_message(
        [this](const nlohmann::json& message) { on_message(message); });
    server->set_on_close([this](const std::string& reason) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (!stopping.load() && error.empty()) error = reason;
        is_running.store(false);
      }
      cv.notify_all();
    });
    std::string listen_error;
    if (!server->listen(endpoint, &listen_error)) {
      set_start_error(listen_error);
      server->close();
      for (int i = 0; i < 8 && uv_loop_alive(&loop); ++i) uv_run(&loop, UV_RUN_NOWAIT);
      uv_loop_close(&loop);
      delete server;
      server = nullptr;
      {
        std::lock_guard<std::mutex> lock(mutex);
        loop_ready = false;
      }
      cv.notify_all();
      return;
    }
    if (uv_async_init(&loop, &wake, on_wake_cb) != 0) {
      set_start_error("could not initialize the listener child command channel");
      server->close();
      for (int i = 0; i < 8 && uv_loop_alive(&loop); ++i) uv_run(&loop, UV_RUN_NOWAIT);
      uv_loop_close(&loop);
      delete server;
      server = nullptr;
      {
        std::lock_guard<std::mutex> lock(mutex);
        loop_ready = false;
      }
      cv.notify_all();
      return;
    }
    wake.data = this;
    {
      std::lock_guard<std::mutex> lock(mutex);
      wake_initialized = true;
    }
    std::string exe = executable;
    if (exe.empty()) {
#if defined(_WIN32)
      exe = (executable_dir(nullptr) / "uvscope.exe").string();
#else
      exe = (executable_dir(nullptr) / "uvscope").string();
#endif
    }
    std::string exe_copy = exe;
    const char* args[] = {exe_copy.c_str(), "--listener-child", endpoint.c_str(), nullptr};
    uv_process_options_t options{};
    options.file = exe_copy.c_str();
    options.args = const_cast<char**>(args);
    options.exit_cb = on_exit_cb;
    const int spawn_rc = uv_spawn(&loop, &process, &options);
    if (spawn_rc != 0) {
      set_start_error(std::string("could not spawn the listener child: ") +
                      uv_strerror(spawn_rc));
      server->close();
      uv_close(reinterpret_cast<uv_handle_t*>(&wake), nullptr);
      for (int i = 0; i < 8 && uv_loop_alive(&loop); ++i) uv_run(&loop, UV_RUN_NOWAIT);
      uv_loop_close(&loop);
      delete server;
      server = nullptr;
      {
        std::lock_guard<std::mutex> lock(mutex);
        wake_initialized = false;
        loop_ready = false;
      }
      cv.notify_all();
      return;
    }
    process.data = this;
    process_spawned = true;
    {
      std::lock_guard<std::mutex> lock(mutex);
      child_pid = process.pid;
      loop_ready = true;
    }
    cv.notify_all();

    uv_run(&loop, UV_RUN_DEFAULT);
    if (server != nullptr) server->close();
    if (wake_initialized) {
      uv_close(reinterpret_cast<uv_handle_t*>(&wake), nullptr);
      wake_initialized = false;
    }
    if (process_spawned && !process_closed) {
      uv_process_kill(&process, SIGTERM);
      for (int i = 0; i < 50 && !process_closed; ++i) {
        uv_run(&loop, UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      if (!process_closed) {
        process_closed = true;
        uv_close(reinterpret_cast<uv_handle_t*>(&process), nullptr);
      }
    }
    for (int i = 0; i < 8 && uv_loop_alive(&loop); ++i) uv_run(&loop, UV_RUN_NOWAIT);
    uv_loop_close(&loop);
    delete server;
    server = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex);
      is_running.store(false);
      loop_ready = false;
    }
    cv.notify_all();
    std::filesystem::remove(endpoint, ec);
  }
};

ListenerChildService::ListenerChildService(std::string executable, ListenerConfig config)
    : impl_(new Impl(std::move(executable), std::move(config))) {}

ListenerChildService::~ListenerChildService() { stop(); }

bool ListenerChildService::start(std::string* error) {
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->thread.joinable()) {
      if (error) *error = "listener child is already started";
      return false;
    }
    impl_->start_result_ready = false;
    impl_->start_ok = false;
    impl_->error.clear();
    impl_->stopping.store(false);
  }
  impl_->thread = std::thread([this] { impl_->loop_main(); });
  std::unique_lock<std::mutex> lock(impl_->mutex);
  if (!impl_->cv.wait_for(lock, std::chrono::seconds(10), [this] {
        return impl_->start_result_ready;
      })) {
    if (error) *error = "listener child start timed out";
    lock.unlock();
    stop();
    return false;
  }
  if (!impl_->start_ok) {
    if (error) *error = impl_->error.empty() ? "listener child failed to start"
                                             : impl_->error;
    return false;
  }
  return true;
}

void ListenerChildService::stop() {
  if (impl_ == nullptr || !impl_->thread.joinable()) return;
  impl_->stopping.store(true);
  if (impl_->is_running.load()) impl_->post({{"type", "stop"}});
  {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->cv.wait_for(lock, std::chrono::seconds(5), [this] {
      return !impl_->is_running.load();
    });
  }
  bool loop_ready = false;
  {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->cv.wait_for(lock, std::chrono::seconds(5), [this] {
      return !impl_->loop_ready;
    });
    loop_ready = impl_->loop_ready;
  }
  if (loop_ready) impl_->post({{"type", "shutdown"}});
  impl_->thread.join();
  impl_->is_running.store(false);
}

bool ListenerChildService::running() const { return impl_->is_running.load(); }

nlohmann::json ListenerChildService::snapshot() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  nlohmann::json out = impl_->snapshot.is_object() ? impl_->snapshot
                                                   : nlohmann::json::object();
  out["id"] = impl_->config.id;
  out["transport"] = impl_->config.transport;
  out["name"] = impl_->config.name;
  out["host"] = impl_->config.host;
  out["port"] = impl_->config.port;
  out["isolated"] = true;
  out["pid"] = impl_->child_pid;
  out["running"] = impl_->is_running.load();
  if (!impl_->is_running.load()) {
    out["state"] = impl_->stopping.load() ? "stopped" : "error";
    if (!impl_->error.empty()) out["error"] = impl_->error;
  }
  return out;
}

nlohmann::json ListenerChildService::events(std::uint64_t after, int limit) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  nlohmann::json out = nlohmann::json::array();
  for (const auto& event : impl_->event_cache) {
    if (!event.is_object() || event.value("seq", 0) <= after) continue;
    out.push_back(event);
    if (static_cast<int>(out.size()) >= std::max(1, limit)) break;
  }
  return out;
}

nlohmann::json ListenerChildService::sessions() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->session_cache;
}

nlohmann::json ListenerChildService::session_events(const std::string& session_id,
                                                    std::uint64_t after, int limit,
                                                    std::string* error) const {
  const auto result = impl_->request(
      {{"type", "session-events"}, {"sessionId", session_id}, {"after", after},
       {"limit", limit}},
      5000, error);
  if (!result.is_object()) return {};
  return result;
}

bool ListenerChildService::send(const std::string& session_id,
                                const std::string& peer_host, int peer_port,
                                const std::string& payload,
                                const std::string& encoding, std::string* error) {
  const auto result = impl_->request(
      {{"type", "send"}, {"sessionId", session_id}, {"peerHost", peer_host},
       {"peerPort", peer_port}, {"payload", payload}, {"encoding", encoding}},
      5000, error);
  if (!result.is_object()) return false;
  if (!result.value("ok", false)) {
    if (error) *error = result.value("error", std::string("listener send failed"));
    return false;
  }
  return true;
}

bool ListenerChildService::close_session(const std::string& session_id,
                                         std::string* error) {
  const auto result = impl_->request(
      {{"type", "close-session"}, {"sessionId", session_id}}, 5000, error);
  if (!result.is_object()) return false;
  if (!result.value("ok", false)) {
    if (error) *error = result.value("error", std::string("listener close failed"));
    return false;
  }
  return true;
}

int ListenerChildService::pid() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->child_pid;
}

}  // namespace uvscope
