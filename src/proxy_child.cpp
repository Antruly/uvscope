#include "proxy_child.h"

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

#include "http_probe.h"
#include "ipc.h"
#include "proxy_manager.h"
#include "util.h"

namespace uvscope {
namespace {

using json = nlohmann::json;

}  // namespace

nlohmann::json proxy_config_to_json(const ProxyConfig& config) {
  return {{"id", config.id},
          {"name", config.name},
          {"listenHost", config.listen_host},
          {"listenPort", config.listen_port},
          {"target", config.target},
          {"upstreams", proxy_upstreams_json(config.upstreams)},
          {"retries", config.retries},
          {"connectTimeoutMs", config.connect_timeout_ms},
          {"requestTimeoutMs", config.request_timeout_ms},
          {"workers", config.workers},
          {"breakerThreshold", config.breaker_threshold},
          {"breakerCooldownMs", config.breaker_cooldown_ms},
          {"isolated", config.isolated},
          {"autoStart", config.auto_start},
          {"tls", config.tls},
          {"certPath", config.cert_path},
          {"keyPath", config.key_path},
          {"rules", proxy_rules_json(config.rules)}};
}

bool proxy_config_from_json(const nlohmann::json& value, ProxyConfig* config,
                            std::string* error) {
  if (!value.is_object() || config == nullptr) {
    if (error) *error = "proxy config must be a JSON object";
    return false;
  }
  ProxyConfig parsed;
  parsed.id = value.value("id", std::string());
  if (parsed.id.empty()) parsed.id = "proxy-child";
  parsed.name = value.value("name", std::string("Proxy child"));
  parsed.listen_host = value.value("listenHost", std::string("127.0.0.1"));
  parsed.listen_port = value.value("listenPort", 0);
  if (parsed.listen_port < 0 || parsed.listen_port > 65535) {
    if (error) *error = "listenPort must be between 0 and 65535";
    return false;
  }
  parsed.target = value.value("target", std::string());
  parsed.auto_start = value.value("autoStart", false);
  parsed.tls = value.value("tls", false);
  parsed.cert_path = value.value("certPath", std::string());
  parsed.key_path = value.value("keyPath", std::string());
  parsed.retries = std::max(0, std::min(value.value("retries", 0), 3));
  parsed.connect_timeout_ms =
      std::max(100, std::min(value.value("connectTimeoutMs", 15000), 120000));
  parsed.request_timeout_ms =
      std::max(100, std::min(value.value("requestTimeoutMs", 30000), 300000));
  parsed.workers = std::max(1, std::min(value.value("workers", 4), 16));
  parsed.breaker_threshold =
      std::max(0, std::min(value.value("breakerThreshold", 5), 100));
  parsed.breaker_cooldown_ms =
      std::max(1000, std::min(value.value("breakerCooldownMs", 30000), 300000));
  parsed.isolated = false;  // a child never spawns another child

  if (value.contains("upstreams")) {
    if (!value["upstreams"].is_array()) {
      if (error) *error = "upstreams must be an array";
      return false;
    }
    for (const auto& item : value["upstreams"]) {
      if (!item.is_object() || parsed.upstreams.size() >= 16) {
        if (error) *error = "invalid upstream entry";
        return false;
      }
      ProxyUpstream upstream;
      upstream.url = item.value("url", std::string());
      upstream.weight = std::max(1, std::min(item.value("weight", 1), 1000));
      const ParsedUrl url = parse_url(upstream.url);
      if (!url.valid || (url.scheme != "http" && url.scheme != "https")) {
        if (error) *error = "upstream " + std::to_string(parsed.upstreams.size() + 1) +
                            " must be an http:// or https:// URL";
        return false;
      }
      parsed.upstreams.push_back(std::move(upstream));
    }
  }
  if (parsed.target.empty() && !parsed.upstreams.empty()) {
    parsed.target = parsed.upstreams.front().url;
  }
  const ParsedUrl target = parse_url(parsed.target);
  if (!target.valid || (target.scheme != "http" && target.scheme != "https")) {
    if (error) *error = "target must be an http:// or https:// URL";
    return false;
  }

  if (value.contains("rules")) {
    if (!value["rules"].is_array()) {
      if (error) *error = "rules must be an array";
      return false;
    }
    for (const auto& item : value["rules"]) {
      if (!item.is_object() || parsed.rules.size() >= 64) {
        if (error) *error = "invalid rule entry";
        return false;
      }
      ProxyRule rule;
      rule.id = item.value("id", std::string());
      rule.name = item.value("name", std::string());
      rule.enabled = item.value("enabled", true);
      rule.match_type = item.value("matchType", std::string("prefix"));
      rule.pattern = item.value("pattern", std::string());
      rule.method = item.value("method", std::string());
      rule.host = item.value("host", std::string());
      rule.header_name = item.value("headerName", std::string());
      rule.header_value = item.value("headerValue", std::string());
      rule.action = item.value("action", std::string("redirect"));
      rule.status = item.value("status", 302);
      rule.destination = item.value("destination", std::string());
      rule.preserve_query = item.value("preserveQuery", true);
      if (rule.match_type != "exact" && rule.match_type != "prefix" &&
          rule.match_type != "regex") {
        if (error) *error = "rule matchType must be exact, prefix or regex";
        return false;
      }
      if (rule.action != "redirect" && rule.action != "rewrite") {
        if (error) *error = "rule action must be redirect or rewrite";
        return false;
      }
      parsed.rules.push_back(std::move(rule));
    }
  }

  *config = std::move(parsed);
  return true;
}

int run_proxy_child(const std::string& endpoint) {
  uv_loop_t loop;
  if (uv_loop_init(&loop) != 0) return 1;
  Store store;
  ProxyManager manager(store, "", false);
  IpcChannel channel(&loop);
  std::string proxy_id;
  struct Telemetry {
    ProxyManager* manager = nullptr;
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
      [&channel, &manager, &proxy_id, &telemetry](const nlohmann::json& message) {
        if (!message.is_object()) return;
        const std::string type = message.value("type", std::string());
        auto reply = [&channel](const nlohmann::json& value) {
          std::string error;
          channel.send(value, &error);
        };
        if (type == "start" || type == "reload") {
          ProxyConfig config;
          std::string error;
          if (!proxy_config_from_json(message.value("config", nlohmann::json::object()),
                                      &config, &error)) {
            reply({{"type", "error"}, {"error", error}});
            return;
          }
          proxy_id = config.id;
          const bool ok = type == "start" ? manager.start_config(config, &error)
                                          : manager.reload_config(config, &error);
          if (!ok) {
            reply({{"type", "error"}, {"error", error}});
            return;
          }
          telemetry.id = proxy_id;
          telemetry.started = true;
          telemetry.last_seq = 0;
          reply({{"type", "started"}, {"snapshot", manager.snapshot()}});
        } else if (type == "snapshot") {
          reply({{"type", "snapshot"}, {"snapshot", manager.snapshot()}});
        } else if (type == "logs") {
          const std::uint64_t after = message.value("after", 0);
          const int limit = std::max(1, std::min(message.value("limit", 200), 500));
          reply({{"type", "logs"}, {"logs", manager.logs(proxy_id, after, limit)}});
        } else if (type == "stop") {
          telemetry.started = false;
          if (!proxy_id.empty()) manager.stop(proxy_id, nullptr);
          reply({{"type", "stopped"}});
          channel.close();
        }
      });
  channel.set_on_close([&loop, &manager, &telemetry](const std::string&) {
    telemetry.started = false;
    manager.stop_all();
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
        const auto snapshots = state->manager->snapshot();
        const auto logs = state->manager->logs(state->id, state->last_seq, 200);
        if (logs.is_array() && !logs.empty()) {
          state->last_seq = logs.back().value("seq", state->last_seq);
        }
        std::string send_error;
        state->channel->send(
            {{"type", "telemetry"}, {"snapshot", snapshots}, {"logs", logs}},
            &send_error);
      },
      500, 500);
  uv_run(&loop, UV_RUN_DEFAULT);
  manager.stop_all();
  channel.close();
  uv_timer_stop(&telemetry_timer);
  uv_close(reinterpret_cast<uv_handle_t*>(&telemetry_timer), nullptr);
  for (int i = 0; i < 8 && uv_loop_alive(&loop); ++i) uv_run(&loop, UV_RUN_NOWAIT);
  uv_loop_close(&loop);
  return 0;
}

bool run_proxy_child_self_check(const std::string& executable, int target_port,
                                std::string* error) {
  uv_loop_t loop;
  if (uv_loop_init(&loop) != 0) {
    if (error) *error = "could not initialize the proxy child self-check loop";
    return false;
  }
  const std::string endpoint = ipc_endpoint_path("proxy-" + random_hex(6));
  std::error_code ec;
  std::filesystem::remove(endpoint, ec);
  uv_loop_t* loop_ptr = &loop;

  struct State {
    bool started = false;
    bool snapshot_seen = false;
    bool stopped = false;
    bool exited = false;
    bool failed = false;
    bool process_closed = false;
    std::string failure;
    uv_process_t* child = nullptr;
  } state;

  IpcChannel server(&loop);
  server.set_on_ready([&server, &state, target_port, loop_ptr] {
    const nlohmann::json config = {
        {"id", "ipc-proxy"},
        {"name", "ipc-proxy"},
        {"listenHost", "127.0.0.1"},
        {"listenPort", 0},
        {"target", "http://127.0.0.1:" + std::to_string(target_port)},
        {"workers", 1},
        {"autoStart", false},
        {"rules", nlohmann::json::array()},
        {"upstreams", nlohmann::json::array()}};
    std::string send_error;
    if (!server.send({{"type", "start"}, {"config", config}}, &send_error)) {
      state.failed = true;
      state.failure = send_error;
      uv_stop(loop_ptr);
    }
  });
  server.set_on_message([&server, &state, loop_ptr](const nlohmann::json& message) {
    if (!message.is_object()) return;
    const std::string type = message.value("type", std::string());
    if (type == "error") {
      state.failed = true;
      state.failure = message.value("error", std::string("proxy child error"));
      uv_stop(loop_ptr);
      return;
    }
    if (type == "started") {
      const auto snapshots = message.value("snapshot", nlohmann::json::array());
      int bound_port = 0;
      if (snapshots.is_array() && !snapshots.empty()) {
        bound_port = snapshots[0].value("boundPort", 0);
      }
      if (bound_port <= 0) {
        state.failed = true;
        state.failure = "proxy child did not report a bound port";
        uv_stop(loop_ptr);
        return;
      }
      HttpProbeInput input;
      input.url = "http://127.0.0.1:" + std::to_string(bound_port) + "/hello";
      input.method = "GET";
      input.timeout_ms = 5000;
      const nlohmann::json result = run_http_probe(input);
      if (!result.value("ok", false) || result.value("status", 0) != 200 ||
          result.value("bodyText", std::string()).find("hello") == std::string::npos) {
        state.failed = true;
        state.failure = "proxy child request failed: " +
                        result.value("error", std::string("unexpected response"));
        uv_stop(loop_ptr);
        return;
      }
      state.started = true;
      std::string send_error;
      server.send({{"type", "snapshot"}}, &send_error);
      return;
    }
    if (type == "snapshot") {
      const auto snapshots = message.value("snapshot", nlohmann::json::array());
      long long requests = 0;
      if (snapshots.is_array() && !snapshots.empty()) {
        requests = snapshots[0].value("requests", 0LL);
      }
      if (requests < 1) {
        state.failed = true;
        state.failure = "proxy child snapshot did not count the request";
        uv_stop(loop_ptr);
        return;
      }
      state.snapshot_seen = true;
      std::string send_error;
      server.send({{"type", "stop"}}, &send_error);
      return;
    }
    if (type == "stopped") {
      state.stopped = true;
      uv_stop(loop_ptr);
    }
  });
  server.set_on_close([](const std::string&) {});

  std::string listen_error;
  if (!server.listen(endpoint, &listen_error)) {
    if (error) *error = listen_error;
    server.close();
    for (int i = 0; i < 8 && uv_loop_alive(&loop); ++i) uv_run(&loop, UV_RUN_NOWAIT);
    uv_loop_close(&loop);
    return false;
  }

  std::string executable_copy = executable;
  const char* args[] = {executable_copy.c_str(), "--proxy-child", endpoint.c_str(), nullptr};
  uv_process_t process;
  uv_process_options_t options{};
  options.file = executable_copy.c_str();
  options.args = const_cast<char**>(args);
  options.exit_cb = [](uv_process_t* child, int64_t, int) {
    auto* s = static_cast<State*>(child->data);
    if (s != nullptr) {
      s->exited = true;
      if (!s->process_closed) {
        s->process_closed = true;
        uv_close(reinterpret_cast<uv_handle_t*>(child), nullptr);
      }
    }
    uv_stop(child->loop);
  };
  const int spawn_rc = uv_spawn(&loop, &process, &options);
  if (spawn_rc != 0) {
    if (error) *error = std::string("could not spawn the proxy child: ") +
                        uv_strerror(spawn_rc);
    server.close();
    for (int i = 0; i < 8 && uv_loop_alive(&loop); ++i) uv_run(&loop, UV_RUN_NOWAIT);
    uv_loop_close(&loop);
    return false;
  }
  process.data = &state;
  state.child = &process;

  uv_timer_t timer;
  uv_timer_init(&loop, &timer);
  timer.data = &state;
  uv_timer_start(&timer, [](uv_timer_t* t) {
    auto* s = static_cast<State*>(t->data);
    s->failed = true;
    s->failure = "proxy child self-check timed out";
    if (s->child != nullptr) uv_process_kill(s->child, SIGTERM);
    uv_stop(t->loop);
  }, 10000, 0);

  uv_run(&loop, UV_RUN_DEFAULT);
  uv_timer_stop(&timer);
  uv_close(reinterpret_cast<uv_handle_t*>(&timer), nullptr);
  server.close();
  if (state.child != nullptr && !state.process_closed) {
    if (!state.exited) uv_process_kill(state.child, SIGTERM);
    state.process_closed = true;
    uv_close(reinterpret_cast<uv_handle_t*>(state.child), nullptr);
  }
  for (int i = 0; i < 8 && uv_loop_alive(&loop); ++i) uv_run(&loop, UV_RUN_NOWAIT);
  uv_loop_close(&loop);
  std::filesystem::remove(endpoint, ec);

  if (state.failed) {
    if (error) *error = state.failure;
    return false;
  }
  if (!state.started) {
    if (error) *error = "proxy child never started";
    return false;
  }
  if (!state.snapshot_seen) {
    if (error) *error = "proxy child snapshot was not observed";
    return false;
  }
  if (!state.stopped) {
    if (error) *error = "proxy child did not acknowledge stop";
    return false;
  }
  return true;
}

struct ProxyChildService::Impl {
  Impl(std::string path, ProxyConfig proxy_config)
      : executable(std::move(path)), config(std::move(proxy_config)) {}

  std::string executable;
  mutable std::mutex mutex;
  std::condition_variable cv;
  ProxyConfig config;
  ProxyConfig pending_config;
  std::uint64_t version = 1;
  uv_loop_t loop{};
  IpcChannel* server = nullptr;
  uv_process_t process{};
  bool process_spawned = false;
  bool process_closed = false;
  uv_async_t wake{};
  bool wake_initialized = false;
  std::deque<nlohmann::json> commands;
  std::thread thread;
  std::atomic<bool> is_running{false};
  std::atomic<bool> stopping{false};
  bool loop_ready = false;
  bool start_result_ready = false;
  bool start_ok = false;
  bool reload_pending = false;
  bool reload_ok = false;
  std::string error;
  std::string reload_error;
  nlohmann::json snapshot = nlohmann::json::object();
  nlohmann::json log_cache = nlohmann::json::array();
  std::uint64_t last_log_seq = 0;
  int child_pid = 0;
  std::string endpoint;

  ProxyConfig config_copy() const {
    std::lock_guard<std::mutex> lock(mutex);
    return config;
  }

  void update_logs(const nlohmann::json& logs) {
    if (!logs.is_array()) return;
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& entry : logs) {
      if (!entry.is_object()) continue;
      const std::uint64_t seq = entry.value("seq", 0);
      if (seq == 0 || seq <= last_log_seq) continue;
      log_cache.push_back(entry);
      last_log_seq = seq;
    }
    while (log_cache.size() > 500) log_cache.erase(log_cache.begin());
  }

  void on_message(const nlohmann::json& message) {
    if (!message.is_object()) return;
    const std::string type = message.value("type", std::string());
    if (type == "started") {
      {
        std::lock_guard<std::mutex> lock(mutex);
        const auto snapshots = message.value("snapshot", nlohmann::json::array());
        if (snapshots.is_array() && !snapshots.empty()) snapshot = snapshots[0];
        is_running.store(true);
        if (reload_pending) {
          reload_pending = false;
          reload_ok = true;
          config = pending_config;
          ++version;
        } else {
          start_ok = true;
          start_result_ready = true;
        }
      }
      cv.notify_all();
      return;
    }
    if (type == "error") {
      const std::string reason =
          message.value("error", std::string("proxy child error"));
      bool fatal = false;
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (reload_pending) {
          reload_pending = false;
          reload_ok = false;
          reload_error = reason;
        } else {
          start_ok = false;
          start_result_ready = true;
          error = reason;
          is_running.store(false);
          fatal = true;
        }
      }
      cv.notify_all();
      if (fatal) uv_stop(&loop);
      return;
    }
    if (type == "telemetry") {
      const auto snapshots = message.value("snapshot", nlohmann::json::array());
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (snapshots.is_array() && !snapshots.empty()) {
          snapshot = snapshots[0];
          is_running.store(true);
        }
      }
      update_logs(message.value("logs", nlohmann::json::array()));
      return;
    }
    if (type == "stopped") {
      {
        std::lock_guard<std::mutex> lock(mutex);
        is_running.store(false);
        stopping.store(true);
      }
      cv.notify_all();
      // Do not stop the loop here: the child process is about to exit and its
      // exit callback must reap it before the loop closes.
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
        self->error = "proxy child exited";
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

  void post(nlohmann::json message) {
    bool signal = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      commands.push_back(std::move(message));
      signal = wake_initialized;
    }
    if (signal) uv_async_send(&wake);
  }

  void loop_main() {
    if (uv_loop_init(&loop) != 0) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        start_ok = false;
        start_result_ready = true;
        error = "could not initialize the proxy child IPC loop";
      }
      cv.notify_all();
      return;
    }
    endpoint = ipc_endpoint_path("proxy-svc-" + random_hex(6));
    std::error_code ec;
    std::filesystem::remove(endpoint, ec);
    server = new IpcChannel(&loop);
    server->set_on_ready([this] {
      std::string send_error;
      if (!server->send(
              {{"type", "start"},
               {"config", proxy_config_to_json(config_copy())}},
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
      // The process exit callback stops the loop after reaping the child.
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
      set_start_error("could not initialize the proxy child command channel");
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
    const char* args[] = {exe_copy.c_str(), "--proxy-child", endpoint.c_str(), nullptr};
    uv_process_options_t options{};
    options.file = exe_copy.c_str();
    options.args = const_cast<char**>(args);
    options.exit_cb = on_exit_cb;
    const int spawn_rc = uv_spawn(&loop, &process, &options);
    if (spawn_rc != 0) {
      set_start_error(std::string("could not spawn the proxy child: ") +
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
      // Give the exit callback a chance to reap the child before the loop and
      // its process handle are closed.
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

ProxyChildService::ProxyChildService(std::string executable, ProxyConfig config)
    : impl_(new Impl(std::move(executable), std::move(config))) {}

ProxyChildService::~ProxyChildService() { stop(); }

bool ProxyChildService::start(std::string* error) {
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->thread.joinable()) {
      if (error) *error = "proxy child is already started";
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
    if (error) *error = "proxy child start timed out";
    lock.unlock();
    stop();
    return false;
  }
  if (!impl_->start_ok) {
    if (error) *error = impl_->error.empty() ? "proxy child failed to start" : impl_->error;
    return false;
  }
  return true;
}

void ProxyChildService::stop() {
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

bool ProxyChildService::reload(const ProxyConfig& config, std::string* error) {
  if (!impl_->is_running.load()) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->config = config;
    ++impl_->version;
    return true;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->reload_pending = true;
    impl_->reload_ok = false;
    impl_->reload_error.clear();
    impl_->pending_config = config;
  }
  impl_->post({{"type", "reload"}, {"config", proxy_config_to_json(config)}});
  std::unique_lock<std::mutex> lock(impl_->mutex);
  if (!impl_->cv.wait_for(lock, std::chrono::seconds(10), [this] {
        return !impl_->reload_pending;
      })) {
    if (error) *error = "proxy child reload timed out";
    return false;
  }
  if (!impl_->reload_ok) {
    if (error) *error = impl_->reload_error.empty() ? "proxy child reload failed"
                                                    : impl_->reload_error;
    return false;
  }
  return true;
}

bool ProxyChildService::running() const { return impl_->is_running.load(); }

nlohmann::json ProxyChildService::snapshot() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  nlohmann::json out = impl_->snapshot.is_object() ? impl_->snapshot
                                                   : nlohmann::json::object();
  out["id"] = impl_->config.id;
  out["name"] = impl_->config.name;
  out["isolated"] = true;
  out["pid"] = impl_->child_pid;
  out["running"] = impl_->is_running.load();
  if (!impl_->error.empty()) out["lastError"] = impl_->error;
  return out;
}

nlohmann::json ProxyChildService::logs(std::uint64_t after, int limit) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  nlohmann::json out = nlohmann::json::array();
  for (const auto& entry : impl_->log_cache) {
    if (!entry.is_object() || entry.value("seq", 0) <= after) continue;
    out.push_back(entry);
    if (static_cast<int>(out.size()) >= std::max(1, limit)) break;
  }
  return out;
}

ProxyConfig ProxyChildService::config_snapshot() const {
  return impl_->config_copy();
}

std::uint64_t ProxyChildService::config_version() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->version;
}

int ProxyChildService::pid() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->child_pid;
}

}  // namespace uvscope
