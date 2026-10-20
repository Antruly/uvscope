#include "socks5_manager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <uv.h>
#include <handle/uvcpp_async.h>
#include <handle/uvcpp_loop.h>
#include <net/uvcpp_socks5_server.h>
#include <net/uvcpp_socks5_types.h>

#include "crypto.h"
#include "util.h"

namespace uvscope {
namespace {

constexpr std::size_t kMaxServers = 16;
constexpr std::size_t kMaxRetainedSessions = 256;
constexpr std::size_t kMaxEvents = 1000;
constexpr std::size_t kMaxConnections = 256;
constexpr int kStartTimeoutMs = 5000;

std::string endpoint_text(const uvcpp::uvcpp_socks5_endpoint& endpoint) {
  if (endpoint.host.empty()) return std::string();
  const bool ipv6 = endpoint.host.find(':') != std::string::npos;
  std::string host = ipv6 ? "[" + endpoint.host + "]" : endpoint.host;
  return host + ":" + std::to_string(endpoint.port);
}

const char* command_name(uvcpp::uvcpp_socks5_command command) {
  switch (command) {
    case uvcpp::uvcpp_socks5_command::BIND:
      return "bind";
    case uvcpp::uvcpp_socks5_command::UDP_ASSOCIATE:
      return "udp-associate";
    case uvcpp::uvcpp_socks5_command::CONNECT:
    default:
      return "connect";
  }
}

}  // namespace

struct Socks5Manager::Impl {
  struct StoredEvent {
    nlohmann::json json;
  };

  struct Server {
    explicit Server(Socks5ServerConfig value) : config(std::move(value)) {}

    Socks5ServerConfig config;

    mutable std::mutex mutex;
    std::string state = "stopped";
    std::string error;
    std::string started_at;
    std::string stopped_at;
    int bound_port = 0;
    std::uint64_t active_sessions = 0;
    std::uint64_t total_sessions = 0;
    std::uint64_t rejected_sessions = 0;
    std::uint64_t next_seq = 1;
    std::deque<StoredEvent> events;
    std::deque<nlohmann::json> sessions;
    // peer endpoint -> session id, used to attach the end callback's stats.
    std::map<std::string, std::string> session_by_peer;

    std::thread thread;
    std::atomic<bool> stop_requested{false};
    std::atomic<uvcpp::uvcpp_async*> wake{nullptr};
    // Read-only cross-thread accessor used for the live session count.
    std::atomic<uvcpp::uvcpp_socks5_server*> runtime{nullptr};

    std::mutex ready_mutex;
    std::condition_variable ready_cv;
    bool ready = false;
  };

  mutable std::mutex map_mutex;
  std::map<std::string, std::shared_ptr<Server>> servers;

  std::shared_ptr<Server> find(const std::string& id) const {
    std::lock_guard<std::mutex> lock(map_mutex);
    const auto it = servers.find(id);
    return it == servers.end() ? nullptr : it->second;
  }

  static void add_event(Server& server, nlohmann::json event) {
    // Caller holds server.mutex.
    event["seq"] = server.next_seq++;
    event["at"] = now_iso8601();
    server.events.push_back({std::move(event)});
    while (server.events.size() > kMaxEvents) server.events.pop_front();
  }

  static nlohmann::json snapshot_locked(const Server& server) {
    const auto* runtime = server.runtime.load();
    const std::uint64_t active =
        runtime != nullptr ? static_cast<std::uint64_t>(runtime->connection_count()) : 0;
    nlohmann::json out = {{"id", server.config.id},
            {"name", server.config.name},
            {"host", server.config.host},
            {"port", server.config.port},
            {"boundPort", server.bound_port},
            {"autoStart", server.config.auto_start},
            {"authRequired", server.config.auth_required},
            {"username", server.config.username},
            {"state", server.state},
            {"error", server.error},
            {"startedAt", server.started_at},
            {"stoppedAt", server.stopped_at},
            {"activeSessions", active},
            {"sessionCount", server.sessions.size()},
            {"totalSessions", server.total_sessions},
            {"rejectedSessions", server.rejected_sessions},
            {"nextSeq", server.next_seq - 1}};
    // Counters arrive with the session-end callback; active sessions
    // legitimately have no numbers yet.
    out["bytesAvailable"] = true;
    return out;
  }

  static void set_ready(Server& server, const std::string& state,
                        const std::string& error) {
    {
      std::lock_guard<std::mutex> lock(server.mutex);
      server.state = state;
      server.error = error;
    }
    {
      std::lock_guard<std::mutex> lock(server.ready_mutex);
      server.ready = true;
    }
    server.ready_cv.notify_all();
  }

  static void on_session(const std::shared_ptr<Server>& server,
                         const uvcpp::uvcpp_socks5_session_info& info) {
    std::lock_guard<std::mutex> lock(server->mutex);
    const bool established = info.status == 0;
    const std::string peer = endpoint_text(info.peer);
    const std::string session_id = make_id();
    nlohmann::json session;
    session["id"] = session_id;
    session["serverId"] = server->config.id;
    session["command"] = command_name(info.command);
    session["target"] = endpoint_text(info.target);
    session["peer"] = peer;
    session["state"] = established ? "established" : "failed";
    session["status"] = info.status;
    session["error"] = established ? std::string() : uv_error_string(info.status);
    session["at"] = now_iso8601();
    if (established) {
      ++server->total_sessions;
    } else {
      ++server->rejected_sessions;
    }
    session["bytesAvailable"] = false;

    nlohmann::json event = {{"type", established ? "session" : "session-failed"},
                            {"sessionId", session["id"]},
                            {"command", session["command"]},
                            {"target", session["target"]},
                            {"peer", session["peer"]},
                            {"status", info.status},
                            {"error", session["error"]}};
    add_event(*server, std::move(event));

    server->sessions.push_back(std::move(session));
    while (server->sessions.size() > kMaxRetainedSessions) {
      server->sessions.pop_front();
    }
    if (!peer.empty()) server->session_by_peer[peer] = session_id;
  }

  static void on_session_end(const std::shared_ptr<Server>& server,
                             const uvcpp::uvcpp_socks5_session_info& info,
                             const uvcpp::uvcpp_socks5_session_stats& stats) {
    std::lock_guard<std::mutex> lock(server->mutex);
    const std::string peer = endpoint_text(info.peer);
    std::string id;
    const auto known = server->session_by_peer.find(peer);
    if (known != server->session_by_peer.end()) {
      id = known->second;
      server->session_by_peer.erase(known);
    }

    nlohmann::json* session = nullptr;
    if (!id.empty()) {
      for (auto& item : server->sessions) {
        if (item.value("id", std::string()) == id) {
          session = &item;
          break;
        }
      }
    }
    if (session == nullptr) {
      // Handshakes that failed before a command result never reached
      // on_session; record the attempt with whatever the callback knows.
      id = make_id();
      nlohmann::json created;
      created["id"] = id;
      created["serverId"] = server->config.id;
      created["command"] = command_name(info.command);
      created["target"] = endpoint_text(info.target);
      created["peer"] = peer;
      created["state"] = stats.end_status == 0 ? "closed" : "error";
      created["status"] = info.status;
      created["error"] = stats.end_status == 0 ? std::string()
                                                : uv_error_string(stats.end_status);
      created["at"] = now_iso8601();
      server->sessions.push_back(std::move(created));
      while (server->sessions.size() > kMaxRetainedSessions) {
        server->sessions.pop_front();
      }
      for (auto& item : server->sessions) {
        if (item.value("id", std::string()) == id) {
          session = &item;
          break;
        }
      }
    }
    if (session == nullptr) return;

    (*session)["state"] = stats.end_status == 0 ? "closed" : "error";
    (*session)["endedAt"] = now_iso8601();
    (*session)["durationMs"] = stats.duration_ms;
    (*session)["bytesClientToTarget"] = stats.bytes_client_to_target;
    (*session)["bytesTargetToClient"] = stats.bytes_target_to_client;
    (*session)["udpDatagrams"] = stats.udp_datagrams;
    (*session)["udpBytes"] = stats.udp_bytes;
    (*session)["endStatus"] = stats.end_status;
    (*session)["bytesAvailable"] = true;
    if (stats.end_status != 0) {
      (*session)["error"] = uv_error_string(stats.end_status);
    }
    add_event(*server, {{"type", "session-end"},
                        {"sessionId", (*session)["id"]},
                        {"command", (*session)["command"]},
                        {"target", (*session)["target"]},
                        {"peer", (*session)["peer"]},
                        {"endStatus", stats.end_status},
                        {"durationMs", stats.duration_ms},
                        {"bytesClientToTarget", stats.bytes_client_to_target},
                        {"bytesTargetToClient", stats.bytes_target_to_client},
                        {"udpDatagrams", stats.udp_datagrams},
                        {"udpBytes", stats.udp_bytes}});
  }

  static void run_server(const std::shared_ptr<Server>& server) {
    auto runtime = std::make_unique<uvcpp::uvcpp_socks5_server>();
    uvcpp::uvcpp_loop* loop = runtime->get_loop();
    if (loop == nullptr) {
      set_ready(*server, "error", "cannot create a SOCKS5 event loop");
      return;
    }

    auto* wake = new uvcpp::uvcpp_async();
    const int wake_rc = wake->init(
        [loop, &runtime, server](uvcpp::uvcpp_async*) {
          if (!server->stop_requested.load()) return;
          {
            std::lock_guard<std::mutex> lock(server->mutex);
            if (server->state == "listening" || server->state == "starting") {
              server->state = "stopping";
            }
          }
          runtime->close();
          loop->stop();
        },
        loop);
    if (wake_rc != 0) {
      delete wake;
      set_ready(*server, "error", "cannot initialize the SOCKS5 wake handle");
      return;
    }
    server->wake.store(wake);

    uvcpp::uvcpp_socks5_server_options options;
    options.allow_no_auth = !server->config.auth_required;
    options.allow_username_password = server->config.auth_required;
    if (server->config.auth_required) {
      const std::string username = server->config.username;
      const std::string hash = server->config.password_hash;
      options.authenticator = [username, hash](const std::string& user,
                                               const std::string& password) {
        if (user != username) {
          // Still run one verification so a wrong username and a wrong
          // password cost the same.
          (void)password_verify(password, hash);
          return false;
        }
        return password_verify(password, hash);
      };
    }
    options.allow_connect = true;
    options.allow_bind = true;
    options.allow_udp_associate = true;
    options.handshake_timeout_ms = 30000;
    options.upstream_connect_timeout_ms = 30000;
    options.bind_accept_timeout_ms = 120000;
    options.max_connections = kMaxConnections;
    runtime->set_options(options);

    const int bind_rc = runtime->bind(server->config.host, server->config.port);
    if (bind_rc != 0) {
      const std::string message = runtime->last_error().empty()
                                      ? uv_error_string(bind_rc)
                                      : runtime->last_error();
      server->wake.store(nullptr);
      delete wake;
      set_ready(*server, "error", message);
      std::lock_guard<std::mutex> lock(server->mutex);
      add_event(*server, {{"type", "error"}, {"error", message}});
      return;
    }

    runtime->set_session_end_callback(
        [server](const uvcpp::uvcpp_socks5_session_info& info,
                 const uvcpp::uvcpp_socks5_session_stats& stats) {
          on_session_end(server, info, stats);
        });
    const int listen_rc = runtime->listen(
        [server](const uvcpp::uvcpp_socks5_session_info& info) {
          on_session(server, info);
        });
    if (listen_rc != 0) {
      const std::string message = runtime->last_error().empty()
                                      ? uv_error_string(listen_rc)
                                      : runtime->last_error();
      server->wake.store(nullptr);
      delete wake;
      set_ready(*server, "error", message);
      std::lock_guard<std::mutex> lock(server->mutex);
      add_event(*server, {{"type", "error"}, {"error", message}});
      return;
    }

    {
      std::lock_guard<std::mutex> lock(server->mutex);
      server->state = "listening";
      server->error.clear();
      server->bound_port = runtime->port();
      server->started_at = now_iso8601();
      server->stopped_at.clear();
      add_event(*server, {{"type", "started"}, {"port", server->bound_port}});
    }
    set_ready(*server, "listening", std::string());
    server->runtime.store(runtime.get());

    runtime->run(UV_RUN_DEFAULT);

    // Back on our own thread: the loop has stopped. Retire the live counter,
    // then tear the runtime down.
    server->runtime.store(nullptr);
    {
      std::lock_guard<std::mutex> lock(server->mutex);
      if (server->state != "error") server->state = "stopped";
      server->stopped_at = now_iso8601();
      server->bound_port = 0;
      add_event(*server, {{"type", "stopped"}});
    }

    server->wake.store(nullptr);
    delete wake;
    runtime.reset();
  }

  void shutdown_server(const std::shared_ptr<Server>& server) {
    if (!server) return;
    if (server->thread.joinable()) {
      server->stop_requested.store(true);
      if (auto* wake = server->wake.load()) wake->send();
      server->thread.join();
    }
  }
};

Socks5Manager::Socks5Manager() : impl_(new Impl()) {}

Socks5Manager::~Socks5Manager() {
  std::vector<std::shared_ptr<Impl::Server>> all;
  {
    std::lock_guard<std::mutex> lock(impl_->map_mutex);
    for (const auto& item : impl_->servers) all.push_back(item.second);
  }
  for (const auto& server : all) impl_->shutdown_server(server);
}

bool Socks5Manager::add_server(const Socks5ServerConfig& config, std::string* error) {
  if (config.id.empty() || config.host.empty() || config.port <= 0 ||
      config.port > 65535) {
    if (error) *error = "invalid SOCKS5 server";
    return false;
  }
  if (config.auth_required &&
      (config.username.empty() || config.password_hash.empty())) {
    if (error) *error = "username and password are required when authentication is on";
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->map_mutex);
  if (impl_->servers.count(config.id) != 0) {
    if (error) *error = "SOCKS5 server already exists";
    return false;
  }
  if (impl_->servers.size() >= kMaxServers) {
    if (error) *error = "at most 16 SOCKS5 servers can be configured";
    return false;
  }
  impl_->servers.emplace(config.id, std::make_shared<Impl::Server>(config));
  return true;
}

bool Socks5Manager::start_server(const std::string& id, std::string* error) {
  auto server = impl_->find(id);
  if (!server) {
    if (error) *error = "SOCKS5 server not found";
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(server->mutex);
    if (server->state == "listening" || server->state == "starting") return true;
  }
  if (server->thread.joinable()) server->thread.join();

  server->stop_requested.store(false);
  {
    std::lock_guard<std::mutex> lock(server->mutex);
    server->state = "starting";
    server->error.clear();
    server->stopped_at.clear();
  }
  {
    std::lock_guard<std::mutex> lock(server->ready_mutex);
    server->ready = false;
  }

  server->thread = std::thread([server] { Impl::run_server(server); });

  bool ready = false;
  {
    std::unique_lock<std::mutex> lock(server->ready_mutex);
    ready = server->ready_cv.wait_for(
        lock, std::chrono::milliseconds(kStartTimeoutMs),
        [&server] { return server->ready; });
  }
  if (!ready) {
    server->stop_requested.store(true);
    if (auto* wake = server->wake.load()) wake->send();
    if (server->thread.joinable()) server->thread.join();
    std::lock_guard<std::mutex> lock(server->mutex);
    server->state = "error";
    server->error = "SOCKS5 server did not start in time";
    if (error) *error = server->error;
    return false;
  }

  std::lock_guard<std::mutex> lock(server->mutex);
  if (server->state != "listening") {
    if (error) *error = server->error.empty() ? "SOCKS5 server failed to start" : server->error;
    return false;
  }
  return true;
}

bool Socks5Manager::stop_server(const std::string& id, std::string* error) {
  auto server = impl_->find(id);
  if (!server) {
    if (error) *error = "SOCKS5 server not found";
    return false;
  }
  if (!server->thread.joinable()) {
    std::lock_guard<std::mutex> lock(server->mutex);
    if (server->state != "error") server->state = "stopped";
    return true;
  }
  impl_->shutdown_server(server);
  std::lock_guard<std::mutex> lock(server->mutex);
  if (server->state == "error") {
    if (error) *error = server->error;
    return true;
  }
  return true;
}

bool Socks5Manager::remove_server(const std::string& id, std::string* error) {
  auto server = impl_->find(id);
  if (!server) {
    if (error) *error = "SOCKS5 server not found";
    return false;
  }
  impl_->shutdown_server(server);
  std::lock_guard<std::mutex> lock(impl_->map_mutex);
  impl_->servers.erase(id);
  return true;
}

nlohmann::json Socks5Manager::servers() const {
  std::vector<std::shared_ptr<Impl::Server>> all;
  {
    std::lock_guard<std::mutex> lock(impl_->map_mutex);
    for (const auto& item : impl_->servers) all.push_back(item.second);
  }
  nlohmann::json out = nlohmann::json::array();
  for (const auto& server : all) {
    std::lock_guard<std::mutex> lock(server->mutex);
    out.push_back(Impl::snapshot_locked(*server));
  }
  std::sort(out.begin(), out.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
    return a.value("id", std::string()) < b.value("id", std::string());
  });
  return {{"ok", true}, {"servers", out}};
}

nlohmann::json Socks5Manager::server(const std::string& id) const {
  auto server = impl_->find(id);
  if (!server) return {{"ok", false}, {"error", "SOCKS5 server not found"}};
  std::lock_guard<std::mutex> lock(server->mutex);
  return {{"ok", true}, {"server", Impl::snapshot_locked(*server)}};
}

nlohmann::json Socks5Manager::server_events(const std::string& id,
                                            std::uint64_t after,
                                            int limit) const {
  auto server = impl_->find(id);
  if (!server) return {{"ok", false}, {"error", "SOCKS5 server not found"}};
  std::lock_guard<std::mutex> lock(server->mutex);
  const int safe_limit = std::max(1, std::min(limit, 500));
  nlohmann::json events = nlohmann::json::array();
  for (const auto& event : server->events) {
    if (event.json.value("seq", std::uint64_t{0}) <= after) continue;
    events.push_back(event.json);
    if (events.size() >= static_cast<std::size_t>(safe_limit)) break;
  }
  return {{"ok", true},
          {"server", Impl::snapshot_locked(*server)},
          {"events", events},
          {"nextSeq", server->next_seq - 1}};
}

nlohmann::json Socks5Manager::server_sessions(const std::string& id) const {
  auto server = impl_->find(id);
  if (!server) return {{"ok", false}, {"error", "SOCKS5 server not found"}};
  std::lock_guard<std::mutex> lock(server->mutex);
  nlohmann::json sessions = nlohmann::json::array();
  for (const auto& session : server->sessions) sessions.push_back(session);
  std::sort(sessions.begin(), sessions.end(),
            [](const nlohmann::json& a, const nlohmann::json& b) {
              return a.value("at", std::string()) > b.value("at", std::string());
            });
  return {{"ok", true}, {"sessions", sessions}};
}

}  // namespace uvscope
