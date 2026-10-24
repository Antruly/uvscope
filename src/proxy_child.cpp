#include "proxy_child.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

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
  ProxyManager manager(store, false);
  IpcChannel channel(&loop);
  std::string proxy_id;

  channel.set_on_ready([&channel] {
    std::string error;
    channel.send({{"type", "ready"}}, &error);
  });
  channel.set_on_message([&channel, &manager, &proxy_id](const nlohmann::json& message) {
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
      reply({{"type", "started"}, {"snapshot", manager.snapshot()}});
    } else if (type == "snapshot") {
      reply({{"type", "snapshot"}, {"snapshot", manager.snapshot()}});
    } else if (type == "logs") {
      const std::uint64_t after = message.value("after", 0);
      const int limit = std::max(1, std::min(message.value("limit", 200), 500));
      reply({{"type", "logs"}, {"logs", manager.logs(proxy_id, after, limit)}});
    } else if (type == "stop") {
      if (!proxy_id.empty()) manager.stop(proxy_id, nullptr);
      reply({{"type", "stopped"}});
      channel.close();
    }
  });
  channel.set_on_close([&loop, &manager](const std::string&) {
    manager.stop_all();
    uv_stop(&loop);
  });
  std::string error;
  if (!channel.connect(endpoint, &error)) {
    uv_loop_close(&loop);
    return 1;
  }
  uv_run(&loop, UV_RUN_DEFAULT);
  manager.stop_all();
  channel.close();
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

}  // namespace uvscope
