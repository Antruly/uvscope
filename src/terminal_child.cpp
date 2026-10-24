#include "terminal_child.h"

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
#include "util.h"

namespace uvscope {

nlohmann::json terminal_config_to_json(const TerminalConfig& config) {
  return {{"name", config.name},
          {"shell", config.shell},
          {"cwd", config.cwd},
          {"cols", config.cols},
          {"rows", config.rows},
          {"isolated", config.isolated}};
}

bool terminal_config_from_json(const nlohmann::json& value, TerminalConfig* config,
                               std::string* error) {
  if (!value.is_object() || config == nullptr) {
    if (error) *error = "terminal config must be a JSON object";
    return false;
  }
  TerminalConfig parsed;
  parsed.name = value.value("name", std::string("Terminal"));
  parsed.shell = value.value("shell", std::string());
  parsed.cwd = value.value("cwd", std::string());
  parsed.cols = std::max(20, std::min(value.value("cols", 100), 500));
  parsed.rows = std::max(5, std::min(value.value("rows", 30), 200));
  parsed.isolated = false;
  if (parsed.name.size() > 80) parsed.name.resize(80);
  *config = std::move(parsed);
  return true;
}

int run_terminal_child(const std::string& endpoint) {
  uv_loop_t loop;
  if (uv_loop_init(&loop) != 0) return 1;
  TerminalManager manager;
  IpcChannel channel(&loop);
  std::string session_id;
  struct Telemetry {
    TerminalManager* manager = nullptr;
    IpcChannel* channel = nullptr;
    std::string id;
    bool started = false;
    std::uint64_t last_seq = 0;
  } telemetry;
  telemetry.manager = &manager;
  telemetry.channel = &channel;

  channel.set_on_ready([&channel] {
    std::string error;
    channel.send({{"type", "ready"}}, &error);
  });
  channel.set_on_message(
      [&channel, &manager, &session_id, &telemetry](const nlohmann::json& message) {
        if (!message.is_object()) return;
        const std::string type = message.value("type", std::string());
        auto reply = [&channel](const nlohmann::json& value) {
          std::string error;
          channel.send(value, &error);
        };
        if (type == "start") {
          TerminalConfig config;
          std::string error;
          if (!terminal_config_from_json(
                  message.value("config", nlohmann::json::object()), &config, &error)) {
            reply({{"type", "error"}, {"error", error}});
            return;
          }
          auto result = manager.create(0, "child", config, &error);
          if (!result.is_object() || !result.value("ok", false)) {
            reply({{"type", "error"},
                   {"error", error.empty() ? "terminal child failed to create a session"
                                           : error}});
            return;
          }
          session_id = result.value("session", nlohmann::json::object())
                           .value("id", std::string());
          telemetry.id = session_id;
          telemetry.started = true;
          telemetry.last_seq = 0;
          reply({{"type", "started"},
                 {"session", result.value("session", nlohmann::json::object())}});
        } else if (type == "input") {
          std::string error;
          const auto bytes = base64_decode(message.value("data", std::string()));
          if (bytes) manager.input(session_id, *bytes, &error);
        } else if (type == "resize") {
          std::string error;
          manager.resize(session_id, message.value("cols", 100),
                         message.value("rows", 30), &error);
        } else if (type == "redraw") {
          std::string error;
          manager.redraw(session_id, &error);
        } else if (type == "close") {
          telemetry.started = false;
          if (!session_id.empty()) manager.close(session_id, nullptr);
          reply({{"type", "closed"}});
          channel.close();
        } else if (type == "stop") {
          telemetry.started = false;
          manager.close_all();
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
        const auto output = state->manager->output(state->id, state->last_seq, 32);
        const auto chunks = output.value("chunks", nlohmann::json::array());
        if (chunks.is_array() && !chunks.empty()) {
          state->last_seq = chunks.back().value("seq", state->last_seq);
        }
        const auto current = state->manager->session(state->id);
        std::string send_error;
        state->channel->send(
            {{"type", "output"},
             {"chunks", chunks},
             {"session", current.value("session", nlohmann::json::object())}},
            &send_error);
      },
      40, 40);
  uv_run(&loop, UV_RUN_DEFAULT);
  manager.close_all();
  channel.close();
  uv_timer_stop(&telemetry_timer);
  uv_close(reinterpret_cast<uv_handle_t*>(&telemetry_timer), nullptr);
  for (int i = 0; i < 8 && uv_loop_alive(&loop); ++i) uv_run(&loop, UV_RUN_NOWAIT);
  uv_loop_close(&loop);
  return 0;
}

struct TerminalChildService::Impl {
  Impl(std::string path, TerminalConfig terminal_config)
      : executable(std::move(path)), config(std::move(terminal_config)) {}

  std::string executable;
  mutable std::mutex lifecycle;  // serializes start/close/stop and thread joins
  mutable std::mutex mutex;
  mutable std::condition_variable cv;
  TerminalConfig config;
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
  bool closed = false;
  bool close_waiting = false;
  std::string error;
  nlohmann::json snapshot = nlohmann::json::object();
  nlohmann::json chunks = nlohmann::json::array();
  std::size_t chunk_bytes = 0;
  std::uint64_t last_chunk_seq = 0;
  int child_pid = 0;
  std::string endpoint;

  TerminalConfig config_copy() const {
    std::lock_guard<std::mutex> lock(mutex);
    return config;
  }

  void update_output(const nlohmann::json& message) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      const auto current = message.value("session", nlohmann::json::object());
      if (current.is_object()) snapshot = current;
      const auto incoming = message.value("chunks", nlohmann::json::array());
      if (incoming.is_array()) {
        for (const auto& chunk : incoming) {
          if (!chunk.is_object()) continue;
          const std::uint64_t seq = chunk.value("seq", 0);
          if (seq == 0 || seq <= last_chunk_seq) continue;
          chunks.push_back(chunk);
          chunk_bytes += chunk.value("data", std::string()).size();
          last_chunk_seq = seq;
        }
        while (chunks.size() > 1024 || chunk_bytes > 256 * 1024) {
          chunk_bytes -= chunks.front().value("data", std::string()).size();
          chunks.erase(chunks.begin());
        }
      }
    }
    cv.notify_all();
  }

  void on_message(const nlohmann::json& message) {
    if (!message.is_object()) return;
    const std::string type = message.value("type", std::string());
    if (type == "started") {
      {
        std::lock_guard<std::mutex> lock(mutex);
        const auto current = message.value("session", nlohmann::json::object());
        if (current.is_object()) snapshot = current;
        is_running.store(true);
        start_ok = true;
        start_result_ready = true;
      }
      cv.notify_all();
      return;
    }
    if (type == "output") {
      update_output(message);
      return;
    }
    if (type == "closed" || type == "stopped") {
      {
        std::lock_guard<std::mutex> lock(mutex);
        closed = true;
        close_waiting = false;
        is_running.store(false);
        stopping.store(true);
      }
      cv.notify_all();
      uv_stop(&loop);
      return;
    }
    if (type == "error") {
      {
        std::lock_guard<std::mutex> lock(mutex);
        start_ok = false;
        start_result_ready = true;
        error = message.value("error", std::string("terminal child error"));
        is_running.store(false);
      }
      cv.notify_all();
      uv_stop(&loop);
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
      self->closed = true;
      if (!self->stopping.load() && self->error.empty()) {
        self->error = "terminal child exited";
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

  void stop_locked() {
    if (!thread.joinable()) return;
    stopping.store(true);
    if (is_running.load()) post({{"type", "stop"}});
    {
      std::unique_lock<std::mutex> lock(mutex);
      cv.wait_for(lock, std::chrono::seconds(5), [this] {
        return !is_running.load();
      });
    }
    bool loop_ready = false;
    {
      std::unique_lock<std::mutex> lock(mutex);
      cv.wait_for(lock, std::chrono::seconds(5), [this] {
        return !this->loop_ready;
      });
      loop_ready = this->loop_ready;
    }
    if (loop_ready) post({{"type", "shutdown"}});
    thread.join();
    is_running.store(false);
  }

  void loop_main() {
    if (uv_loop_init(&loop) != 0) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        start_ok = false;
        start_result_ready = true;
        error = "could not initialize the terminal child IPC loop";
      }
      cv.notify_all();
      return;
    }
    endpoint = ipc_endpoint_path("terminal-svc-" + random_hex(6));
    std::error_code ec;
    std::filesystem::remove(endpoint, ec);
    server = new IpcChannel(&loop);
    server->set_on_ready([this] {
      std::string send_error;
      if (!server->send(
              {{"type", "start"}, {"config", terminal_config_to_json(config_copy())}},
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
        closed = true;
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
      set_start_error("could not initialize the terminal child command channel");
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
    const char* args[] = {exe_copy.c_str(), "--terminal-child", endpoint.c_str(), nullptr};
    uv_process_options_t options{};
    options.file = exe_copy.c_str();
    options.args = const_cast<char**>(args);
    options.exit_cb = on_exit_cb;
    const int spawn_rc = uv_spawn(&loop, &process, &options);
    if (spawn_rc != 0) {
      set_start_error(std::string("could not spawn the terminal child: ") +
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
      closed = true;
    }
    cv.notify_all();
    std::filesystem::remove(endpoint, ec);
  }
};

TerminalChildService::TerminalChildService(std::string executable, TerminalConfig config)
    : impl_(new Impl(std::move(executable), std::move(config))) {}

TerminalChildService::~TerminalChildService() { stop(); }

bool TerminalChildService::start(std::string* error) {
  std::lock_guard<std::mutex> lifecycle(impl_->lifecycle);
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->thread.joinable()) {
      if (error) *error = "terminal child is already started";
      return false;
    }
    impl_->start_result_ready = false;
    impl_->start_ok = false;
    impl_->error.clear();
    impl_->stopping.store(false);
    impl_->closed = false;
  }
  impl_->thread = std::thread([this] { impl_->loop_main(); });
  std::unique_lock<std::mutex> lock(impl_->mutex);
  if (!impl_->cv.wait_for(lock, std::chrono::seconds(10), [this] {
        return impl_->start_result_ready;
      })) {
    if (error) *error = "terminal child start timed out";
    lock.unlock();
    impl_->stop_locked();
    return false;
  }
  if (!impl_->start_ok) {
    if (error) *error = impl_->error.empty() ? "terminal child failed to start"
                                             : impl_->error;
    return false;
  }
  return true;
}

void TerminalChildService::close() {
  std::lock_guard<std::mutex> lifecycle(impl_->lifecycle);
  if (impl_->is_running.load()) {
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      impl_->close_waiting = true;
    }
    impl_->post({{"type", "close"}});
    {
      std::unique_lock<std::mutex> lock(impl_->mutex);
      impl_->cv.wait_for(lock, std::chrono::seconds(5), [this] {
        return impl_->closed || !impl_->is_running.load();
      });
    }
  }
  impl_->stop_locked();
}

void TerminalChildService::stop() {
  if (impl_ == nullptr) return;
  std::lock_guard<std::mutex> lifecycle(impl_->lifecycle);
  impl_->stop_locked();
}

bool TerminalChildService::running() const { return impl_->is_running.load(); }

bool TerminalChildService::input(const std::string& bytes, std::string* error) {
  if (bytes.size() > 64 * 1024) {
    if (error) *error = "terminal input exceeds 64 KiB";
    return false;
  }
  if (!impl_->is_running.load()) {
    if (error) *error = "terminal child is not running";
    return false;
  }
  impl_->post({{"type", "input"}, {"data", base64_encode(bytes)}});
  return true;
}

bool TerminalChildService::resize(int cols, int rows, std::string* error) {
  if (!impl_->is_running.load()) {
    if (error) *error = "terminal child is not running";
    return false;
  }
  impl_->post({{"type", "resize"}, {"cols", cols}, {"rows", rows}});
  return true;
}

bool TerminalChildService::redraw(std::string* error) {
  if (!impl_->is_running.load()) {
    if (error) *error = "terminal child is not running";
    return false;
  }
  impl_->post({{"type", "redraw"}});
  return true;
}

nlohmann::json TerminalChildService::snapshot() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  nlohmann::json out = impl_->snapshot.is_object() ? impl_->snapshot
                                                   : nlohmann::json::object();
  out["isolated"] = true;
  out["pid"] = impl_->child_pid;
  out["running"] = impl_->is_running.load();
  if (!impl_->is_running.load()) {
    if (out.value("state", std::string()) != "exited") {
      // A supervisor-requested stop keeps the session and must not look like
      // a crash; an unexpected child exit still does.
      out["state"] = impl_->stopping.load() ? "stopped" : "error";
    }
    if (!impl_->error.empty()) out["error"] = impl_->error;
  }
  return out;
}

nlohmann::json TerminalChildService::output(std::uint64_t after, int limit) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const int safe_limit = std::max(1, std::min(limit, 500));
  nlohmann::json out = nlohmann::json::array();
  std::uint64_t first_seq = 0;
  for (const auto& chunk : impl_->chunks) {
    if (!chunk.is_object()) continue;
    const std::uint64_t seq = chunk.value("seq", std::uint64_t{0});
    if (seq <= after) continue;
    if (first_seq == 0) first_seq = seq;
    out.push_back(chunk);
    if (static_cast<int>(out.size()) >= safe_limit) break;
  }
  const std::uint64_t front =
      impl_->chunks.is_array() && !impl_->chunks.empty()
          ? impl_->chunks.front().value("seq", 0)
          : 0;
  // `last_chunk_seq < after` means the client is ahead of this child: the
  // terminal was restarted and its sequence numbers started over. Treat that
  // as missed so the WebSocket client resets before replaying new output.
  const bool stale =
      after > 0 && (front > after + 1 || impl_->last_chunk_seq < after);
  return {{"ok", true},
          {"chunks", out},
          {"firstSeq", first_seq},
          {"nextSeq", impl_->last_chunk_seq},
          {"truncated", front > 1},
          {"missed", stale}};
}

int TerminalChildService::pid() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->child_pid;
}

}  // namespace uvscope
