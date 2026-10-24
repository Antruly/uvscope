#include "transport_manager.h"

#include "listener_child.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <uv.h>
#include <handle/uvcpp_async.h>
#include <handle/uvcpp_loop.h>
#include <net/uvcpp_net_read.h>
#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>

#include "util.h"

namespace uvscope {
namespace {

constexpr std::size_t kMaxSessions = 64;
constexpr std::size_t kMaxRetainedSessions = 128;
constexpr std::size_t kMaxPayload = 1024 * 1024;
constexpr std::size_t kMaxQueuedBytes = 1024 * 1024;
constexpr std::size_t kMaxEventPayload = 64 * 1024;
constexpr std::size_t kMaxEventBytes = 512 * 1024;
constexpr std::size_t kMaxEventCount = 2000;

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool decode_payload(const std::string& payload, const std::string& encoding,
                    std::string* bytes, std::string* error) {
  if (lower(encoding) == "hex") {
    const auto decoded = hex_decode(payload);
    if (!decoded) {
      if (error) *error = "payload is not valid hex";
      return false;
    }
    *bytes = *decoded;
  } else {
    *bytes = payload;
  }
  if (bytes->size() > kMaxPayload) {
    if (error) *error = "payload exceeds the 1 MiB session limit";
    return false;
  }
  return true;
}

nlohmann::json bytes_json(const std::string& bytes) {
  const std::size_t kept_size = std::min(bytes.size(), kMaxEventPayload);
  const std::string kept = bytes.substr(0, kept_size);
  const bool printable = std::all_of(kept.begin(), kept.end(), [](unsigned char c) {
    return c == '\n' || c == '\r' || c == '\t' || (c >= 0x20 && c < 0x7F);
  });
  nlohmann::json out;
  out["length"] = bytes.size();
  out["truncated"] = kept_size != bytes.size();
  out["hex"] = hex_encode(kept, true);
  out["encoding"] = printable ? "utf8" : "base64";
  if (printable) {
    out["text"] = kept;
  } else {
    out["base64"] = base64_encode(kept);
  }
  return out;
}

const char* state_name(const std::string& state) { return state.c_str(); }

}  // namespace

struct TransportManager::Impl {
  struct StoredEvent {
    nlohmann::json json;
    std::size_t bytes = 0;
  };

  struct Session {
    std::string id;
    std::string host;
    int port = 0;
    int connect_timeout_ms = 15000;
    std::string state = "connecting";
    std::string error;
    std::string close_reason;
    std::string created_at;
    std::string connected_at;
    std::string closed_at;
    std::int64_t created_ms = 0;
    std::uint64_t sent_bytes = 0;
    std::uint64_t received_bytes = 0;
    std::uint64_t next_seq = 1;
    std::size_t queued_writes = 0;
    std::size_t queued_bytes = 0;
    Socks5Proxy proxy;
    std::string proxy_error_class;
    std::deque<StoredEvent> events;
    std::size_t event_bytes = 0;
  };

  struct Active {
    uvcpp::uvcpp_tcp_client* client = nullptr;
    bool writing = false;
    std::string current_payload;
    std::deque<std::pair<std::string, std::size_t>> writes;
    std::size_t queued_bytes = 0;
  };

  struct Accepted {
    std::string id;
    std::string peer;
    std::string state = "connected";
    std::string error;
    std::string connected_at;
    std::string closed_at;
    uvcpp::uvcpp_tcp_client* client = nullptr;
    std::uint64_t sent_bytes = 0;
    std::uint64_t received_bytes = 0;
    std::uint64_t next_seq = 1;
    bool writing = false;
    std::string current_payload;
    std::deque<std::pair<std::string, std::size_t>> writes;
    std::size_t queued_bytes = 0;
    std::deque<StoredEvent> events;
    std::size_t event_bytes = 0;
  };

  struct Listener {
    ListenerConfig config;
    std::shared_ptr<ListenerChildService> child;
    std::string state = "stopped";
    std::string error;
    std::string started_at;
    std::string stopped_at;
    std::uint64_t next_seq = 1;
    std::uint64_t datagrams = 0;
    std::string last_peer;
    std::deque<StoredEvent> events;
    std::size_t event_bytes = 0;
    uvcpp::uvcpp_tcp_server* tcp = nullptr;
    uv_udp_t* udp = nullptr;
    std::map<std::string, std::shared_ptr<Accepted>> sessions;
  };

  struct UdpContext {
    Impl* impl = nullptr;
    std::string listener_id;
  };

  struct UdpSendContext {
    Impl* impl = nullptr;
    std::string listener_id;
    std::string peer;
    std::string payload;
    uv_udp_send_t req{};
  };

  std::thread thread;
  std::mutex ready_mutex;
  std::condition_variable ready_cv;
  bool ready = false;
  std::string start_error;

  uvcpp::uvcpp_loop* loop = nullptr;
  uvcpp::uvcpp_async* wake = nullptr;
  uv_timer_t* sweep_timer = nullptr;

  mutable std::mutex command_mutex;
  bool accepting = true;
  std::vector<std::function<void()>> commands;

  std::string executable_path;
  mutable std::mutex state_mutex;
  std::map<std::string, std::shared_ptr<Session>> sessions;

  std::map<std::string, std::shared_ptr<Listener>> listeners;

  // Loop-thread only.
  std::map<std::string, std::unique_ptr<Active>> active;
  std::vector<uvcpp::uvcpp_tcp_client*> retired;
  std::vector<uvcpp::uvcpp_tcp_server*> retired_servers;

  bool post(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(command_mutex);
    if (!accepting) return false;
    commands.push_back(std::move(fn));
    if (wake != nullptr) wake->send();
    return true;
  }

  void drain() {
    std::vector<std::function<void()>> local;
    {
      std::lock_guard<std::mutex> lock(command_mutex);
      local.swap(commands);
    }
    for (auto& fn : local) fn();
  }

  std::shared_ptr<Session> session_locked(const std::string& id) const {
    const auto it = sessions.find(id);
    return it == sessions.end() ? nullptr : it->second;
  }

  void add_event_locked(Session& session, nlohmann::json event) {
    event["seq"] = session.next_seq++;
    event["at"] = now_iso8601();
    const std::string serialized = event.dump();
    session.events.push_back({std::move(event), serialized.size()});
    session.event_bytes += serialized.size();
    while (session.events.size() > kMaxEventCount ||
           session.event_bytes > kMaxEventBytes) {
      session.event_bytes -= session.events.front().bytes;
      session.events.pop_front();
    }
  }

  void add_event(const std::string& id, nlohmann::json event) {
    std::lock_guard<std::mutex> lock(state_mutex);
    auto session = session_locked(id);
    if (!session) return;
    add_event_locked(*session, std::move(event));
  }

  nlohmann::json snapshot_locked(const Session& session) const {
    return {{"id", session.id},
            {"transport", "tcp"},
            {"host", session.host},
            {"port", session.port},
            {"state", state_name(session.state)},
            {"error", session.error},
            {"closeReason", session.close_reason},
            {"createdAt", session.created_at},
            {"connectedAt", session.connected_at},
            {"closedAt", session.closed_at},
            {"sentBytes", session.sent_bytes},
            {"receivedBytes", session.received_bytes},
            {"nextSeq", session.next_seq - 1},
            {"queuedWrites", session.queued_writes},
            {"queuedBytes", session.queued_bytes},
            {"proxy", socks5_proxy_json(session.proxy)},
            {"proxyErrorClass", session.proxy_error_class}};
  }

  void append_event(std::deque<StoredEvent>& events, std::size_t& bytes,
                    std::uint64_t& seq, nlohmann::json event) {
    event["seq"] = seq++;
    event["at"] = now_iso8601();
    const std::string serialized = event.dump();
    events.push_back({std::move(event), serialized.size()});
    bytes += serialized.size();
    while (events.size() > kMaxEventCount || bytes > kMaxEventBytes) {
      bytes -= events.front().bytes;
      events.pop_front();
    }
  }

  std::shared_ptr<Listener> listener_locked(const std::string& id) const {
    const auto it = listeners.find(id);
    return it == listeners.end() ? nullptr : it->second;
  }

  void listener_event_locked(Listener& listener, nlohmann::json event) {
    append_event(listener.events, listener.event_bytes, listener.next_seq,
                 std::move(event));
  }

  void accepted_event_locked(Accepted& session, nlohmann::json event) {
    append_event(session.events, session.event_bytes, session.next_seq,
                 std::move(event));
  }

  std::string peer_string(uvcpp::uvcpp_tcp_client* client) const {
    if (client == nullptr) return std::string();
    uvcpp::uvcpp_tcp* tcp = client->get_tcp();
    if (tcp == nullptr) return std::string();
    struct sockaddr_storage storage;
    std::memset(&storage, 0, sizeof(storage));
    int len = static_cast<int>(sizeof(storage));
    if (tcp->getpeername(reinterpret_cast<struct sockaddr*>(&storage), &len) != 0) {
      return std::string();
    }
    char ip[INET6_ADDRSTRLEN] = {0};
    int port = 0;
    if (storage.ss_family == AF_INET) {
      auto* addr = reinterpret_cast<struct sockaddr_in*>(&storage);
      uv_ip4_name(addr, ip, sizeof(ip));
      port = ntohs(addr->sin_port);
    } else if (storage.ss_family == AF_INET6) {
      auto* addr = reinterpret_cast<struct sockaddr_in6*>(&storage);
      uv_ip6_name(addr, ip, sizeof(ip));
      port = ntohs(addr->sin6_port);
    } else {
      return std::string();
    }
    return std::string(ip) + ":" + std::to_string(port);
  }

  nlohmann::json accepted_snapshot_locked(const Accepted& session) const {
    return {{"id", session.id},
            {"peer", session.peer},
            {"state", state_name(session.state)},
            {"error", session.error},
            {"connectedAt", session.connected_at},
            {"closedAt", session.closed_at},
            {"sentBytes", session.sent_bytes},
            {"receivedBytes", session.received_bytes},
            {"nextSeq", session.next_seq - 1},
            {"queuedWrites", session.writes.size() + (session.writing ? 1 : 0)},
            {"queuedBytes", session.queued_bytes}};
  }

  nlohmann::json listener_snapshot_locked(const Listener& listener) const {
    std::size_t active_sessions = 0;
    for (const auto& item : listener.sessions) {
      if (item.second->state == "connected") ++active_sessions;
    }
    return {{"id", listener.config.id},
            {"transport", listener.config.transport},
            {"name", listener.config.name},
            {"host", listener.config.host},
            {"port", listener.config.port},
            {"autoStart", listener.config.auto_start},
            {"isolated", listener.config.isolated},
            {"pid", 0},
            {"state", state_name(listener.state)},
            {"error", listener.error},
            {"startedAt", listener.started_at},
            {"stoppedAt", listener.stopped_at},
            {"sessionCount", listener.sessions.size()},
            {"activeSessions", active_sessions},
            {"datagrams", listener.datagrams},
            {"lastPeer", listener.last_peer},
            {"nextSeq", listener.next_seq - 1}};
  }

  void mark_connected(const std::string& id) {
    std::lock_guard<std::mutex> lock(state_mutex);
    auto session = session_locked(id);
    if (!session || session->state != "connecting") return;
    session->state = "connected";
    session->connected_at = now_iso8601();
    add_event_locked(*session, {{"type", "connected"}});
  }

  void mark_error(const std::string& id, const std::string& message) {
    std::lock_guard<std::mutex> lock(state_mutex);
    auto session = session_locked(id);
    if (!session || session->state == "error" || session->state == "closed") return;
    session->state = "error";
    session->error = message;
    session->closed_at = now_iso8601();
    add_event_locked(*session, {{"type", "error"}, {"error", message}});
  }

  void mark_closing(const std::string& id, const std::string& reason) {
    std::lock_guard<std::mutex> lock(state_mutex);
    auto session = session_locked(id);
    if (!session || session->state == "closed") return;
    if (session->state != "error") session->state = "closing";
    session->close_reason = reason;
  }

  void on_client_close(const std::string& id) {
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      auto session = session_locked(id);
      if (session && session->state != "error") {
        session->state = "closed";
        if (session->closed_at.empty()) session->closed_at = now_iso8601();
        add_event_locked(*session, {{"type", "closed"},
                                    {"reason", session->close_reason}});
      } else if (session) {
        add_event_locked(*session, {{"type", "closed"},
                                    {"reason", session->close_reason}});
      }
    }
    const auto it = active.find(id);
    if (it == active.end()) return;
    retired.push_back(it->second->client);
    active.erase(it);
  }

  void start_connect(const std::string& id, const std::string& host, int port,
                     int timeout_ms, const Socks5Proxy& proxy) {
    std::shared_ptr<Session> session;
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      session = session_locked(id);
    }
    if (!session || session->state != "connecting") return;

    auto client = new uvcpp::uvcpp_tcp_client(loop);
    std::unique_ptr<Active> holder(new Active());
    holder->client = client;
    active[id] = std::move(holder);

    client->set_on_close([this, id]() { on_client_close(id); });

    if (proxy.enabled()) {
      const int proxy_rc = client->set_socks5_proxy(
          proxy.host, proxy.port, proxy.username, proxy.password);
      if (proxy_rc != 0) {
        {
          std::lock_guard<std::mutex> lock(state_mutex);
          auto session = session_locked(id);
          if (session) session->proxy_error_class = "invalid_config";
        }
        mark_error(id, "invalid SOCKS5 proxy: " + uv_error_string(proxy_rc));
        close_active(id, "invalid SOCKS5 proxy");
        return;
      }
    }

    const int rc = client->connect(host.c_str(), port, [this, id, client](int status) {
      if (status != 0) {
        std::string message = "connect failed: " + uv_error_string(status);
        std::string error_class;
        if (client->socks5_enabled()) {
          const std::string detail = client->socks5_last_error();
          if (!detail.empty()) message = "connect failed: " + detail;
          error_class = socks5_error_class(status, detail,
                                           client->socks5_proxy_connect_failed());
        }
        bool closing = false;
        {
          std::lock_guard<std::mutex> lock(state_mutex);
          auto session = session_locked(id);
          closing = session && session->state == "closing";
          if (session && !closing) session->proxy_error_class = error_class;
        }
        if (!closing) {
          mark_error(id, message);
        }
        close_active(id, "connect failed");
        return;
      }
      mark_connected(id);
      const auto it = active.find(id);
      if (it == active.end()) return;
      const int rrc = it->second->client->read_start_events(
          [this, id](uvcpp::uvcpp_tcp_client&, const uvcpp::net_read_result& result) {
            on_read(id, result);
          });
      if (rrc != 0) {
        mark_error(id, "read start failed: " + uv_error_string(rrc));
        close_active(id, "read start failed");
      }
    });
    if (rc != 0) {
      const std::string detail = client->socks5_enabled() ? client->socks5_last_error()
                                                          : std::string();
      {
        std::lock_guard<std::mutex> lock(state_mutex);
        auto session = session_locked(id);
        if (session && client->socks5_enabled()) {
          session->proxy_error_class = socks5_error_class(
              rc, detail, client->socks5_proxy_connect_failed());
        }
      }
      mark_error(id, "connect could not start: " +
                         (detail.empty() ? uv_error_string(rc) : detail));
      close_active(id, "connect could not start");
    }
    (void)timeout_ms;
  }

  void on_read(const std::string& id, const uvcpp::net_read_result& result) {
    if (result.event == uvcpp::net_read_event::DATA) {
      if (result.data == nullptr || result.size == 0) return;
      const std::string bytes(result.data, result.size);
      {
        std::lock_guard<std::mutex> lock(state_mutex);
        auto session = session_locked(id);
        if (!session) return;
        session->received_bytes += bytes.size();
        add_event_locked(*session, {{"type", "recv"}, {"payload", bytes_json(bytes)}});
      }
      return;
    }

    if (result.event == uvcpp::net_read_event::PEER_CLOSED) {
      {
        std::lock_guard<std::mutex> lock(state_mutex);
        auto session = session_locked(id);
        if (session) {
          session->state = result.fin ? "closed" : "error";
          if (!result.fin) session->error = "peer reset the connection";
          session->closed_at = now_iso8601();
          session->close_reason = result.fin ? "peer closed" : "peer reset";
          add_event_locked(*session, {{"type", "peer-closed"},
                                      {"fin", result.fin}});
        }
      }
      close_active(id, result.fin ? "peer closed" : "peer reset");
      return;
    }

    mark_error(id, "read failed: " + uv_error_string(result.error));
    close_active(id, "read failed");
  }

  void enqueue_write(const std::string& id, std::string payload) {
    const auto it = active.find(id);
    if (it == active.end()) {
      add_event(id, {{"type", "error"}, {"error", "session is not connected"}});
      return;
    }
    Active& entry = *it->second;
    if (entry.queued_bytes + payload.size() > kMaxQueuedBytes) {
      add_event(id, {{"type", "error"}, {"error", "send queue is full"}});
      return;
    }
    entry.queued_bytes += payload.size();
    const std::size_t size = payload.size();
    entry.writes.emplace_back(std::move(payload), size);
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      auto session = session_locked(id);
      if (session) {
        session->queued_writes = entry.writes.size() + (entry.writing ? 1 : 0);
        session->queued_bytes = entry.queued_bytes;
      }
    }
    pump_write(id);
  }

  void pump_write(const std::string& id) {
    const auto it = active.find(id);
    if (it == active.end()) return;
    Active& entry = *it->second;
    if (entry.writing || entry.writes.empty()) {
      if (!entry.writing) {
        std::lock_guard<std::mutex> lock(state_mutex);
        auto session = session_locked(id);
        if (session) {
          session->queued_writes = 0;
          session->queued_bytes = 0;
        }
      }
      return;
    }
    if (entry.client == nullptr) return;

    entry.current_payload = std::move(entry.writes.front().first);
    const std::size_t size = entry.writes.front().second;
    entry.queued_bytes -= size;
    entry.writes.pop_front();
    entry.writing = true;
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      auto session = session_locked(id);
      if (session) {
        session->queued_writes = entry.writes.size() + 1;
        session->queued_bytes = entry.queued_bytes;
      }
    }

    const int rc = entry.client->write(
        entry.current_payload.data(), entry.current_payload.size(),
        [this, id, size](int status) { on_write(id, size, status); });
    if (rc != 0) {
      entry.writing = false;
      mark_error(id, "write failed: " + uv_error_string(rc));
      close_active(id, "write failed");
    }
  }

  void on_write(const std::string& id, std::size_t size, int status) {
    const auto it = active.find(id);
    if (it == active.end()) return;
    Active& entry = *it->second;
    entry.writing = false;
    if (status == 0) {
      std::lock_guard<std::mutex> lock(state_mutex);
      auto session = session_locked(id);
      if (session) {
        session->sent_bytes += size;
        add_event_locked(*session, {{"type", "send"},
                                    {"payload", bytes_json(entry.current_payload)}});
      }
    } else {
      mark_error(id, "write failed: " + uv_error_string(status));
      close_active(id, "write failed");
      return;
    }
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      auto session = session_locked(id);
      if (session) {
        session->queued_writes = entry.writes.size();
        session->queued_bytes = entry.queued_bytes;
      }
    }
    pump_write(id);
  }

  void close_active(const std::string& id, const std::string& reason) {
    const auto it = active.find(id);
    if (it == active.end()) return;
    mark_closing(id, reason);
    uvcpp::uvcpp_tcp_client* client = it->second->client;
    if (client == nullptr) {
      active.erase(it);
      return;
    }
    const int rc = client->close();
    if (rc != 0) {
      retired.push_back(client);
      active.erase(it);
      mark_error(id, "close failed: " + uv_error_string(rc));
    }
  }

  void mark_listener_error(const std::string& id, const std::string& message) {
    std::lock_guard<std::mutex> lock(state_mutex);
    auto listener = listener_locked(id);
    if (!listener || listener->state == "error") return;
    listener->state = "error";
    listener->error = message;
    listener->stopped_at = now_iso8601();
    listener_event_locked(*listener, {{"type", "error"}, {"error", message}});
  }

  void mark_listener_listening(const std::string& id) {
    std::lock_guard<std::mutex> lock(state_mutex);
    auto listener = listener_locked(id);
    if (!listener) return;
    listener->state = "listening";
    listener->error.clear();
    listener->started_at = now_iso8601();
    listener_event_locked(*listener, {{"type", "started"}});
  }

  void start_listener_loop(const std::string& id) {
    std::shared_ptr<Listener> listener;
    ListenerConfig config;
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      listener = listener_locked(id);
      if (!listener || listener->state == "listening") return;
      listener->state = "starting";
      listener->error.clear();
      config = listener->config;
    }

    if (config.transport == "tcp") {
      auto* server = new uvcpp::uvcpp_tcp_server(loop);
      server->set_tcp_nodelay(true);
      int rc = server->bind(config.host.c_str(), config.port);
      if (rc == 0) {
        rc = server->listen([this, id](uvcpp::uvcpp_tcp_client* client) {
          on_accept(id, client);
        });
      }
      if (rc != 0) {
        delete server;
        mark_listener_error(id, "bind/listen failed: " + uv_error_string(rc));
        return;
      }
      {
        std::lock_guard<std::mutex> lock(state_mutex);
        auto current = listener_locked(id);
        if (!current) {
          retired_servers.push_back(server);
          return;
        }
        current->tcp = server;
      }
      mark_listener_listening(id);
      return;
    }

    auto* udp = new uv_udp_t();
    udp->data = new UdpContext{this, id};
    int rc = uv_udp_init(reinterpret_cast<uv_loop_t*>(loop->get_handle()), udp);
    struct sockaddr_storage storage;
    std::memset(&storage, 0, sizeof(storage));
    if (rc == 0) {
      struct sockaddr_in addr4;
      struct sockaddr_in6 addr6;
      if (uv_ip4_addr(config.host.c_str(), config.port, &addr4) == 0) {
        std::memcpy(&storage, &addr4, sizeof(addr4));
      } else if (uv_ip6_addr(config.host.c_str(), config.port, &addr6) == 0) {
        std::memcpy(&storage, &addr6, sizeof(addr6));
      } else {
        rc = UV_EINVAL;
      }
    }
    if (rc == 0) {
      rc = uv_udp_bind(udp, reinterpret_cast<const struct sockaddr*>(&storage), 0);
    }
    if (rc == 0) {
      rc = uv_udp_recv_start(udp, &Impl::udp_alloc_cb, &Impl::udp_recv_cb);
    }
    if (rc != 0) {
      uv_close(reinterpret_cast<uv_handle_t*>(udp), [](uv_handle_t* handle) {
        delete static_cast<UdpContext*>(handle->data);
        delete reinterpret_cast<uv_udp_t*>(handle);
      });
      mark_listener_error(id, "bind/receive failed: " + uv_error_string(rc));
      return;
    }
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      auto current = listener_locked(id);
      if (!current) {
        uv_udp_recv_stop(udp);
        uv_close(reinterpret_cast<uv_handle_t*>(udp), [](uv_handle_t* handle) {
          delete static_cast<UdpContext*>(handle->data);
          delete reinterpret_cast<uv_udp_t*>(handle);
        });
        return;
      }
      current->udp = udp;
    }
    mark_listener_listening(id);
  }

  void stop_listener_loop(const std::string& id) {
    std::shared_ptr<Listener> listener;
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      listener = listener_locked(id);
      if (!listener) return;
    }

    if (listener->tcp != nullptr) {
      uvcpp::uvcpp_tcp_server* server = listener->tcp;
      listener->tcp = nullptr;
      server->stop();
      server->close_all_clients();
      {
        std::lock_guard<std::mutex> lock(state_mutex);
        for (auto& item : listener->sessions) {
          Accepted& session = *item.second;
          if (session.state != "closed" && session.state != "error") {
            session.state = "closed";
            session.closed_at = now_iso8601();
            accepted_event_locked(session, {{"type", "closed"},
                                            {"reason", "listener stopped"}});
            listener_event_locked(*listener, {{"type", "session-closed"},
                                              {"sessionId", session.id}});
          }
          session.client = nullptr;
        }
      }
      retired_servers.push_back(server);
    }

    if (listener->udp != nullptr) {
      uv_udp_t* udp = listener->udp;
      listener->udp = nullptr;
      uv_udp_recv_stop(udp);
      uv_close(reinterpret_cast<uv_handle_t*>(udp), [](uv_handle_t* handle) {
        delete static_cast<UdpContext*>(handle->data);
        delete reinterpret_cast<uv_udp_t*>(handle);
      });
    }

    {
      std::lock_guard<std::mutex> lock(state_mutex);
      listener->state = "stopped";
      listener->stopped_at = now_iso8601();
      listener_event_locked(*listener, {{"type", "stopped"}});
    }
  }

  void on_accept(const std::string& listener_id, uvcpp::uvcpp_tcp_client* client) {
    auto session = std::make_shared<Accepted>();
    session->id = make_id();
    session->peer = peer_string(client);
    session->connected_at = now_iso8601();
    session->client = client;
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      auto listener = listener_locked(listener_id);
      if (!listener) {
        client->close();
        return;
      }
      accepted_event_locked(*session, {{"type", "accepted"}, {"peer", session->peer}});
      listener->sessions[session->id] = session;
      listener_event_locked(*listener, {{"type", "accepted"},
                                        {"sessionId", session->id},
                                        {"peer", session->peer}});
      while (listener->sessions.size() > 128) {
        auto oldest = listener->sessions.end();
        for (auto it = listener->sessions.begin(); it != listener->sessions.end(); ++it) {
          if (it->second->state != "closed" && it->second->state != "error") continue;
          if (oldest == listener->sessions.end() ||
              it->second->connected_at < oldest->second->connected_at) {
            oldest = it;
          }
        }
        if (oldest == listener->sessions.end()) break;
        listener->sessions.erase(oldest);
      }
    }
    const std::string session_id = session->id;
    client->set_on_close([this, listener_id, session_id] {
      on_accepted_close(listener_id, session_id);
    });
    const int rc = client->read_start_events(
        [this, listener_id, session_id](uvcpp::uvcpp_tcp_client&,
                                        const uvcpp::net_read_result& result) {
          on_accepted_read(listener_id, session_id, result);
        });
    if (rc != 0) {
      std::lock_guard<std::mutex> lock(state_mutex);
      auto listener = listener_locked(listener_id);
      if (!listener) return;
      auto it = listener->sessions.find(session_id);
      if (it != listener->sessions.end()) {
        it->second->state = "error";
        it->second->error = "read start failed: " + uv_error_string(rc);
        accepted_event_locked(*it->second, {{"type", "error"},
                                            {"error", it->second->error}});
      }
      client->close();
    }
  }

  void on_accepted_close(const std::string& listener_id,
                         const std::string& session_id) {
    std::lock_guard<std::mutex> lock(state_mutex);
    auto listener = listener_locked(listener_id);
    if (!listener) return;
    auto it = listener->sessions.find(session_id);
    if (it == listener->sessions.end()) return;
    Accepted& session = *it->second;
    session.client = nullptr;
    if (session.state == "closed" || session.state == "error") return;
    session.state = "closed";
    session.closed_at = now_iso8601();
    accepted_event_locked(session, {{"type", "closed"}});
    listener_event_locked(*listener, {{"type", "session-closed"},
                                      {"sessionId", session.id}});
  }

  void on_accepted_read(const std::string& listener_id,
                        const std::string& session_id,
                        const uvcpp::net_read_result& result) {
    if (result.event == uvcpp::net_read_event::DATA) {
      if (result.data == nullptr || result.size == 0) return;
      const std::string bytes(result.data, result.size);
      std::lock_guard<std::mutex> lock(state_mutex);
      auto listener = listener_locked(listener_id);
      if (!listener) return;
      auto it = listener->sessions.find(session_id);
      if (it == listener->sessions.end()) return;
      it->second->received_bytes += bytes.size();
      accepted_event_locked(*it->second, {{"type", "recv"},
                                          {"payload", bytes_json(bytes)}});
      return;
    }

    std::lock_guard<std::mutex> lock(state_mutex);
    auto listener = listener_locked(listener_id);
    if (!listener) return;
    auto it = listener->sessions.find(session_id);
    if (it == listener->sessions.end()) return;
    Accepted& session = *it->second;
    if (result.event == uvcpp::net_read_event::PEER_CLOSED) {
      session.state = result.fin ? "closed" : "error";
      if (!result.fin) session.error = "peer reset the connection";
      session.closed_at = now_iso8601();
      accepted_event_locked(session, {{"type", "peer-closed"}, {"fin", result.fin}});
      listener_event_locked(*listener, {{"type", "session-closed"},
                                        {"sessionId", session.id}});
    } else {
      session.state = "error";
      session.error = "read failed: " + uv_error_string(result.error);
      session.closed_at = now_iso8601();
      accepted_event_locked(session, {{"type", "error"}, {"error", session.error}});
      listener_event_locked(*listener, {{"type", "session-closed"},
                                        {"sessionId", session.id}});
    }
  }

  void enqueue_accepted_write(const std::string& listener_id,
                              const std::string& session_id,
                              std::string payload) {
    std::lock_guard<std::mutex> lock(state_mutex);
    auto listener = listener_locked(listener_id);
    if (!listener) return;
    auto it = listener->sessions.find(session_id);
    if (it == listener->sessions.end() || it->second->client == nullptr ||
        it->second->state != "connected") {
      if (it != listener->sessions.end()) {
        accepted_event_locked(*it->second, {{"type", "error"},
                                            {"error", "session is not connected"}});
      }
      return;
    }
    Accepted& session = *it->second;
    if (session.queued_bytes + payload.size() > kMaxQueuedBytes) {
      accepted_event_locked(session, {{"type", "error"},
                                      {"error", "send queue is full"}});
      return;
    }
    const std::size_t size = payload.size();
    session.queued_bytes += size;
    session.writes.emplace_back(std::move(payload), size);
    pump_accepted_write_locked(listener_id, session_id, session);
  }

  void pump_accepted_write_locked(const std::string& listener_id,
                                  const std::string& session_id,
                                  Accepted& session) {
    if (session.writing || session.writes.empty() || session.client == nullptr) return;
    session.current_payload = std::move(session.writes.front().first);
    const std::size_t size = session.current_payload.size();
    session.queued_bytes -= size;
    session.writes.pop_front();
    session.writing = true;
    const int rc = session.client->write(
        session.current_payload.data(), session.current_payload.size(),
        [this, listener_id, session_id, size](int status) {
          on_accepted_write(listener_id, session_id, size, status);
        });
    if (rc != 0) {
      session.writing = false;
      session.state = "error";
      session.error = "write failed: " + uv_error_string(rc);
      accepted_event_locked(session, {{"type", "error"}, {"error", session.error}});
      session.client->close();
    }
  }

  void on_accepted_write(const std::string& listener_id,
                         const std::string& session_id, std::size_t size,
                         int status) {
    std::lock_guard<std::mutex> lock(state_mutex);
    auto listener = listener_locked(listener_id);
    if (!listener) return;
    auto it = listener->sessions.find(session_id);
    if (it == listener->sessions.end()) return;
    Accepted& session = *it->second;
    session.writing = false;
    if (status == 0) {
      session.sent_bytes += size;
      accepted_event_locked(session, {{"type", "send"},
                                      {"payload", bytes_json(session.current_payload)}});
      pump_accepted_write_locked(listener_id, session_id, session);
    } else {
      session.state = "error";
      session.error = "write failed: " + uv_error_string(status);
      accepted_event_locked(session, {{"type", "error"}, {"error", session.error}});
      if (session.client != nullptr) session.client->close();
    }
  }

  static void udp_alloc_cb(uv_handle_t*, std::size_t suggested, uv_buf_t* buf) {
    char* data = static_cast<char*>(std::malloc(suggested));
    *buf = uv_buf_init(data, data == nullptr ? 0 : static_cast<unsigned int>(suggested));
  }

  static void udp_recv_cb(uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf,
                          const struct sockaddr* addr, unsigned flags) {
    auto* context = static_cast<UdpContext*>(handle->data);
    if (context != nullptr && nread > 0 && addr != nullptr && buf->base != nullptr) {
      context->impl->on_udp_datagram(context->listener_id, buf->base,
                                     static_cast<std::size_t>(nread), addr,
                                     (flags & UV_UDP_PARTIAL) != 0);
    }
    std::free(buf->base);
  }

  void on_udp_datagram(const std::string& listener_id, const char* data,
                       std::size_t size, const struct sockaddr* addr, bool partial) {
    char ip[INET6_ADDRSTRLEN] = {0};
    int port = 0;
    if (addr->sa_family == AF_INET) {
      auto* in = reinterpret_cast<const struct sockaddr_in*>(addr);
      uv_ip4_name(in, ip, sizeof(ip));
      port = ntohs(in->sin_port);
    } else if (addr->sa_family == AF_INET6) {
      auto* in6 = reinterpret_cast<const struct sockaddr_in6*>(addr);
      uv_ip6_name(in6, ip, sizeof(ip));
      port = ntohs(in6->sin6_port);
    } else {
      return;
    }
    const std::string peer = std::string(ip) + ":" + std::to_string(port);
    const std::string bytes(data, size);
    std::lock_guard<std::mutex> lock(state_mutex);
    auto listener = listener_locked(listener_id);
    if (!listener) return;
    listener->datagrams += 1;
    listener->last_peer = peer;
    listener_event_locked(*listener, {{"type", "datagram"},
                                      {"peer", peer},
                                      {"partial", partial},
                                      {"payload", bytes_json(bytes)}});
  }

  bool send_udp(const std::string& listener_id, const std::string& peer_host,
                int peer_port, const std::string& payload, std::string* error) {
    std::shared_ptr<Listener> listener;
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      listener = listener_locked(listener_id);
      if (!listener) {
        if (error) *error = "listener not found";
        return false;
      }
      if (listener->udp == nullptr || listener->state != "listening") {
        if (error) *error = "listener is not running";
        return false;
      }
    }
    struct sockaddr_storage storage;
    std::memset(&storage, 0, sizeof(storage));
    struct sockaddr_in addr4;
    struct sockaddr_in6 addr6;
    const struct sockaddr* addr = nullptr;
    if (uv_ip4_addr(peer_host.c_str(), peer_port, &addr4) == 0) {
      std::memcpy(&storage, &addr4, sizeof(addr4));
      addr = reinterpret_cast<const struct sockaddr*>(&storage);
    } else if (uv_ip6_addr(peer_host.c_str(), peer_port, &addr6) == 0) {
      std::memcpy(&storage, &addr6, sizeof(addr6));
      addr = reinterpret_cast<const struct sockaddr*>(&storage);
    } else {
      if (error) *error = "peer address must be a numeric IP";
      return false;
    }

    auto* context = new UdpSendContext();
    context->impl = this;
    context->listener_id = listener_id;
    context->peer = std::string(peer_host) + ":" + std::to_string(peer_port);
    context->payload = payload;
    context->req.data = context;
    uv_buf_t buf = uv_buf_init(const_cast<char*>(context->payload.data()),
                               static_cast<unsigned int>(context->payload.size()));
    const int rc = uv_udp_send(
        &context->req, listener->udp, &buf, 1, addr,
        [](uv_udp_send_t* req, int status) {
          auto* send = static_cast<UdpSendContext*>(req->data);
          if (send != nullptr) {
            send->impl->on_udp_sent(send->listener_id, send->peer,
                                    send->payload, status);
            delete send;
          }
        });
    if (rc != 0) {
      delete context;
      if (error) *error = "send failed: " + uv_error_string(rc);
      return false;
    }
    return true;
  }

  void on_udp_sent(const std::string& listener_id, const std::string& peer,
                   const std::string& payload, int status) {
    std::lock_guard<std::mutex> lock(state_mutex);
    auto listener = listener_locked(listener_id);
    if (!listener) return;
    if (status == 0) {
      listener_event_locked(*listener, {{"type", "sent"},
                                        {"peer", peer},
                                        {"payload", bytes_json(payload)}});
    } else {
      listener_event_locked(*listener, {{"type", "error"},
                                        {"error", "send failed: " + uv_error_string(status)}});
    }
  }

  void close_accepted(const std::string& listener_id,
                      const std::string& session_id) {
    uvcpp::uvcpp_tcp_client* client = nullptr;
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      auto listener = listener_locked(listener_id);
      if (!listener) return;
      auto it = listener->sessions.find(session_id);
      if (it == listener->sessions.end()) return;
      client = it->second->client;
      if (client != nullptr && it->second->state == "connected") {
        it->second->state = "closing";
      }
    }
    if (client != nullptr) client->close();
  }

  void sweep() {
    for (uvcpp::uvcpp_tcp_client* client : retired) {
      if (client != nullptr) delete client;
    }
    retired.clear();

    for (uvcpp::uvcpp_tcp_server* server : retired_servers) {
      if (server != nullptr) delete server;
    }
    retired_servers.clear();

    const std::int64_t now = now_ms();
    std::vector<std::pair<std::string, std::string>> expired;
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      for (const auto& item : sessions) {
        const auto& session = *item.second;
        if (session.state != "connecting") continue;
        if (now - session.created_ms > session.connect_timeout_ms) {
          expired.emplace_back(session.id, "connect timed out");
        }
      }
    }
    for (const auto& item : expired) {
      mark_error(item.first, item.second);
      close_active(item.first, item.second);
    }
  }

  void shutdown_loop() {
    std::vector<std::string> listener_ids;
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      for (const auto& item : listeners) {
        if (item.second->state == "listening" || item.second->state == "starting") {
          listener_ids.push_back(item.first);
        }
      }
    }
    for (const auto& id : listener_ids) stop_listener_loop(id);

    std::vector<std::string> ids;
    for (const auto& item : active) ids.push_back(item.first);
    for (const auto& id : ids) close_active(id, "shutdown");

    if (sweep_timer != nullptr) {

      uv_timer_stop(sweep_timer);
      uv_close(reinterpret_cast<uv_handle_t*>(sweep_timer),
               [](uv_handle_t* handle) {

                 delete reinterpret_cast<uv_timer_t*>(handle);
               });
      sweep_timer = nullptr;
    }
    if (wake != nullptr) {

      wake->close();
    }

  }

  void run_loop() {
    auto fail = [this](const std::string& message) {
      {
        std::lock_guard<std::mutex> lock(ready_mutex);
        start_error = message;
        ready = true;
      }
      ready_cv.notify_all();
    };

    // uvcpp_loop's constructor already initializes the loop; calling init()
    // again would run uv_loop_init on live memory.
    auto* created_loop = new uvcpp::uvcpp_loop();
    loop = created_loop;

    // The default constructor only resets the wrapper; the overload below
    // performs the single uv_async_init with our callback.
    auto* created_wake = new uvcpp::uvcpp_async();
    if (created_wake->init([this](uvcpp::uvcpp_async*) { drain(); }, loop) != 0) {
      delete created_wake;
      loop->loop_close();
      delete loop;
      loop = nullptr;
      fail("cannot initialize the transport wake handle");
      return;
    }
    wake = created_wake;

    auto* timer = new uv_timer_t();
    timer->data = this;
    if (uv_timer_init(reinterpret_cast<uv_loop_t*>(loop->get_handle()), timer) != 0) {
      delete timer;
      delete wake;
      wake = nullptr;
      loop->loop_close();
      delete loop;
      loop = nullptr;
      fail("cannot initialize the transport timer");
      return;
    }
    sweep_timer = timer;
    uv_timer_start(timer,
                   [](uv_timer_t* handle) {
                     static_cast<Impl*>(handle->data)->sweep();
                   },
                   500, 500);

    {
      std::lock_guard<std::mutex> lock(ready_mutex);
      ready = true;
    }
    ready_cv.notify_all();

    loop->run(UV_RUN_DEFAULT);


    for (auto& item : active) {
      delete item.second->client;
    }
    active.clear();
    for (auto* client : retired) delete client;
    retired.clear();
    for (auto* server : retired_servers) delete server;
    retired_servers.clear();
    for (auto& item : listeners) {
      if (item.second->tcp != nullptr) {
        delete item.second->tcp;
        item.second->tcp = nullptr;
      }
      if (item.second->udp != nullptr) {
        uv_udp_recv_stop(item.second->udp);
        uv_close(reinterpret_cast<uv_handle_t*>(item.second->udp),
                 [](uv_handle_t* handle) {
                   delete static_cast<UdpContext*>(handle->data);
                   delete reinterpret_cast<uv_udp_t*>(handle);
                 });
        item.second->udp = nullptr;
      }
    }

    if (sweep_timer != nullptr) {
      uv_close(reinterpret_cast<uv_handle_t*>(sweep_timer),
               [](uv_handle_t* handle) {
                 delete reinterpret_cast<uv_timer_t*>(handle);
               });
      sweep_timer = nullptr;
    }
    if (wake != nullptr) {
      delete wake;
      wake = nullptr;
    }
    loop->loop_close();
    delete loop;
    loop = nullptr;
  }
};

TransportManager::TransportManager(std::string executable_path) : impl_(new Impl()) {
  impl_->executable_path = std::move(executable_path);
  impl_->thread = std::thread([this] { impl_->run_loop(); });
  std::unique_lock<std::mutex> lock(impl_->ready_mutex);
  impl_->ready_cv.wait(lock, [this] { return impl_->ready; });
}

TransportManager::~TransportManager() {

  if (!impl_->thread.joinable()) return;
  {
    std::lock_guard<std::mutex> lock(impl_->command_mutex);
    impl_->accepting = false;
    impl_->commands.push_back([this] { impl_->shutdown_loop(); });
  }
  if (impl_->wake != nullptr) impl_->wake->send();

  impl_->thread.join();

}

bool TransportManager::available() const {
  return impl_ != nullptr && impl_->start_error.empty();
}

const std::string& TransportManager::start_error() const {
  return impl_->start_error;
}

nlohmann::json TransportManager::create_tcp_session(const std::string& host,
                                                    int port,
                                                    int connect_timeout_ms,
                                                    const Socks5Proxy& proxy,
                                                    std::string* error) {
  if (!available()) {
    if (error) *error = start_error().empty() ? "transport manager is not running" : start_error();
    return {{"ok", false}, {"error", error ? *error : "transport manager is not running"}};
  }
  if (host.empty() || port <= 0 || port > 65535) {
    if (error) *error = "host and port are required";
    return {{"ok", false}, {"error", "host and port are required"}};
  }
  std::string proxy_error;
  if (!proxy.valid(&proxy_error)) {
    if (error) *error = proxy_error;
    return {{"ok", false}, {"error", proxy_error}, {"code", "invalid_proxy"}};
  }
  const int timeout = std::max(500, std::min(connect_timeout_ms, 120000));

  auto session = std::make_shared<Impl::Session>();
  session->id = make_id();
  session->host = host;
  session->port = port;
  session->connect_timeout_ms = timeout;
  session->proxy = proxy;
  session->created_ms = now_ms();
  session->created_at = now_iso8601();

  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    std::size_t live = 0;
    for (const auto& item : impl_->sessions) {
      if (item.second->state != "closed" && item.second->state != "error") ++live;
    }
    if (live >= kMaxSessions) {
      if (error) *error = "too many live TCP sessions";
      return {{"ok", false}, {"error", "too many live TCP sessions"}};
    }
    impl_->add_event_locked(*session, {{"type", "created"}});
    impl_->sessions[session->id] = session;
    while (impl_->sessions.size() > kMaxRetainedSessions) {
      auto oldest = impl_->sessions.end();
      for (auto it = impl_->sessions.begin(); it != impl_->sessions.end(); ++it) {
        if (it->second->state != "closed" && it->second->state != "error") continue;
        if (oldest == impl_->sessions.end() ||
            it->second->created_ms < oldest->second->created_ms) {
          oldest = it;
        }
      }
      if (oldest == impl_->sessions.end()) break;
      impl_->sessions.erase(oldest);
    }
  }

  const std::string id = session->id;
  if (!impl_->post([this, id, host, port, timeout, proxy] {
        impl_->start_connect(id, host, port, timeout, proxy);
      })) {
    {
      std::lock_guard<std::mutex> lock(impl_->state_mutex);
      impl_->sessions.erase(id);
    }
    if (error) *error = "transport manager is stopping";
    return {{"ok", false}, {"error", "transport manager is stopping"}};
  }

  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  auto stored = impl_->session_locked(id);
  return {{"ok", true}, {"session", impl_->snapshot_locked(*stored)}};
}

nlohmann::json TransportManager::tcp_sessions() const {
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  nlohmann::json out = nlohmann::json::array();
  for (const auto& item : impl_->sessions) {
    out.push_back(impl_->snapshot_locked(*item.second));
  }
  std::sort(out.begin(), out.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
    return a.value("createdAt", std::string()) > b.value("createdAt", std::string());
  });
  return {{"ok", true}, {"sessions", out}};
}

nlohmann::json TransportManager::tcp_session(const std::string& id) const {
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  auto session = impl_->session_locked(id);
  if (!session) return {{"ok", false}, {"error", "session not found"}};
  return {{"ok", true}, {"session", impl_->snapshot_locked(*session)}};
}

nlohmann::json TransportManager::tcp_session_events(const std::string& id,
                                                    std::uint64_t after,
                                                    int limit) const {
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  auto session = impl_->session_locked(id);
  if (!session) return {{"ok", false}, {"error", "session not found"}};

  const int safe_limit = std::max(1, std::min(limit, 500));
  nlohmann::json events = nlohmann::json::array();
  for (const auto& event : session->events) {
    if (event.json.value("seq", std::uint64_t{0}) <= after) continue;
    events.push_back(event.json);
    if (events.size() >= static_cast<std::size_t>(safe_limit)) break;
  }
  return {{"ok", true},
          {"session", impl_->snapshot_locked(*session)},
          {"events", events},
          {"nextSeq", session->next_seq - 1}};
}

bool TransportManager::tcp_send(const std::string& id, const std::string& payload,
                                const std::string& encoding, std::string* error) {
  std::string bytes;
  if (!decode_payload(payload, encoding, &bytes, error)) return false;
  if (bytes.empty()) {
    if (error) *error = "payload is empty";
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    auto session = impl_->session_locked(id);
    if (!session) {
      if (error) *error = "session not found";
      return false;
    }
    if (session->state != "connected") {
      if (error) *error = "session is not connected";
      return false;
    }
  }

  if (!impl_->post([this, id, bytes] { impl_->enqueue_write(id, bytes); })) {
    if (error) *error = "transport manager is stopping";
    return false;
  }
  return true;
}

bool TransportManager::tcp_close(const std::string& id, std::string* error) {
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    auto session = impl_->session_locked(id);
    if (!session) {
      if (error) *error = "session not found";
      return false;
    }
    if (session->state == "closed") return true;
  }
  if (!impl_->post([this, id] { impl_->close_active(id, "closed by user"); })) {
    if (error) *error = "transport manager is stopping";
    return false;
  }
  return true;
}

bool TransportManager::add_listener(const ListenerConfig& config,
                                    std::string* error) {
  if (!available()) {
    if (error) *error = start_error().empty() ? "transport manager is not running" : start_error();
    return false;
  }
  if (config.id.empty() || (config.transport != "tcp" && config.transport != "udp") ||
      config.host.empty() || config.port <= 0 || config.port > 65535) {
    if (error) *error = "invalid listener";
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  auto existing = impl_->listener_locked(config.id);
  if (existing) {
    if ((existing->state == "listening" || existing->state == "starting") &&
        (existing->config.transport != config.transport ||
         existing->config.host != config.host || existing->config.port != config.port)) {
      if (error) *error = "stop the listener before changing transport, host or port";
      return false;
    }
    existing->config = config;
    return true;
  }
  if (impl_->listeners.size() >= 32) {
    if (error) *error = "too many listeners";
    return false;
  }
  auto listener = std::make_shared<Impl::Listener>();
  listener->config = config;
  impl_->listeners[config.id] = listener;
  return true;
}

bool TransportManager::listener_running(const std::string& id) const {
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  const auto listener = impl_->listener_locked(id);
  if (!listener) return false;
  if (listener->config.isolated) {
    return listener->child && listener->child->running();
  }
  // "starting" counts as alive so the supervisor does not enqueue a second
  // start while the transport loop is still binding.
  return listener->state == "listening" || listener->state == "starting";
}

bool TransportManager::start_listener(const std::string& id, std::string* error) {
  std::shared_ptr<Impl::Listener> listener;
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    listener = impl_->listener_locked(id);
    if (!listener) {
      if (error) *error = "listener not found";
      return false;
    }
    if (listener->config.isolated) {
      if (listener->child && listener->child->running()) return true;
    } else {
      if (listener->state == "listening") return true;
      listener->state = "starting";
    }
  }
  if (listener->config.isolated) {
    listener->child.reset(
        new ListenerChildService(impl_->executable_path, listener->config));
    std::string child_error;
    if (!listener->child->start(&child_error)) {
      std::lock_guard<std::mutex> lock(impl_->state_mutex);
      listener->state = "error";
      listener->error = child_error.empty() ? "listener child failed to start"
                                            : child_error;
      listener->child.reset();
      if (error) *error = listener->error;
      return false;
    }
    const auto snapshot = listener->child->snapshot();
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    listener->state = snapshot.value("state", std::string("listening"));
    listener->error.clear();
    listener->started_at = now_iso8601();
    return true;
  }
  if (!impl_->post([this, id] { impl_->start_listener_loop(id); })) {
    if (error) *error = "transport manager is stopping";
    return false;
  }
  return true;
}

bool TransportManager::stop_listener(const std::string& id, std::string* error) {
  std::shared_ptr<Impl::Listener> listener;
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    listener = impl_->listener_locked(id);
    if (!listener) {
      if (error) *error = "listener not found";
      return false;
    }
    if (!listener->config.isolated && listener->state == "stopped") return true;
  }
  if (listener->config.isolated) {
    if (listener->child) {
      listener->child->stop();
      listener->child.reset();
    }
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    listener->state = "stopped";
    listener->stopped_at = now_iso8601();
    return true;
  }
  if (!impl_->post([this, id] { impl_->stop_listener_loop(id); })) {
    if (error) *error = "transport manager is stopping";
    return false;
  }
  return true;
}

bool TransportManager::remove_listener(const std::string& id, std::string* error) {
  std::shared_ptr<Impl::Listener> listener;
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    listener = impl_->listener_locked(id);
    if (!listener) {
      if (error) *error = "listener not found";
      return false;
    }
  }
  if (listener->config.isolated) {
    if (listener->child) {
      listener->child->stop();
      listener->child.reset();
    }
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    impl_->listeners.erase(id);
    return true;
  }
  if (!impl_->post([this, id] {
        impl_->stop_listener_loop(id);
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        impl_->listeners.erase(id);
      })) {
    if (error) *error = "transport manager is stopping";
    return false;
  }
  return true;
}

nlohmann::json TransportManager::listeners() const {
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  nlohmann::json out = nlohmann::json::array();
  for (const auto& item : impl_->listeners) {
    if (item.second->child) {
      out.push_back(item.second->child->snapshot());
    } else {
      out.push_back(impl_->listener_snapshot_locked(*item.second));
    }
  }
  std::sort(out.begin(), out.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
    return a.value("id", std::string()) < b.value("id", std::string());
  });
  return {{"ok", true}, {"listeners", out}};
}

nlohmann::json TransportManager::listener(const std::string& id) const {
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  auto listener = impl_->listener_locked(id);
  if (!listener) return {{"ok", false}, {"error", "listener not found"}};
  if (listener->child) {
    return {{"ok", true}, {"listener", listener->child->snapshot()}};
  }
  return {{"ok", true}, {"listener", impl_->listener_snapshot_locked(*listener)}};
}

nlohmann::json TransportManager::listener_events(const std::string& id,
                                                 std::uint64_t after,
                                                 int limit) const {
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  auto listener = impl_->listener_locked(id);
  if (!listener) return {{"ok", false}, {"error", "listener not found"}};
  const int safe_limit = std::max(1, std::min(limit, 500));
  if (listener->child) {
    const auto events = listener->child->events(after, safe_limit);
    const std::uint64_t next =
        events.is_array() && !events.empty() ? events.back().value("seq", after) : after;
    return {{"ok", true},
            {"listener", listener->child->snapshot()},
            {"events", events},
            {"nextSeq", next}};
  }
  nlohmann::json events = nlohmann::json::array();
  for (const auto& event : listener->events) {
    if (event.json.value("seq", std::uint64_t{0}) <= after) continue;
    events.push_back(event.json);
    if (events.size() >= static_cast<std::size_t>(safe_limit)) break;
  }
  return {{"ok", true},
          {"listener", impl_->listener_snapshot_locked(*listener)},
          {"events", events},
          {"nextSeq", listener->next_seq - 1}};
}

nlohmann::json TransportManager::listener_sessions(const std::string& id) const {
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  auto listener = impl_->listener_locked(id);
  if (!listener) return {{"ok", false}, {"error", "listener not found"}};
  if (listener->child) {
    return {{"ok", true}, {"sessions", listener->child->sessions()}};
  }
  nlohmann::json sessions = nlohmann::json::array();
  for (const auto& item : listener->sessions) {
    auto snapshot = impl_->accepted_snapshot_locked(*item.second);
    snapshot["listenerId"] = id;
    sessions.push_back(snapshot);
  }
  std::sort(sessions.begin(), sessions.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
    return a.value("connectedAt", std::string()) > b.value("connectedAt", std::string());
  });
  return {{"ok", true}, {"sessions", sessions}};
}

nlohmann::json TransportManager::listener_session_events(
    const std::string& id, const std::string& session_id, std::uint64_t after,
    int limit) const {
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  auto listener = impl_->listener_locked(id);
  if (!listener) return {{"ok", false}, {"error", "listener not found"}};
  if (listener->child) {
    std::string error;
    auto result = listener->child->session_events(session_id, after, limit, &error);
    if (!result.is_object()) {
      return {{"ok", false}, {"error", error.empty() ? "session not found" : error}};
    }
    return result;
  }
  auto it = listener->sessions.find(session_id);
  if (it == listener->sessions.end()) {
    return {{"ok", false}, {"error", "session not found"}};
  }
  const int safe_limit = std::max(1, std::min(limit, 500));
  nlohmann::json events = nlohmann::json::array();
  for (const auto& event : it->second->events) {
    if (event.json.value("seq", std::uint64_t{0}) <= after) continue;
    events.push_back(event.json);
    if (events.size() >= static_cast<std::size_t>(safe_limit)) break;
  }
  return {{"ok", true},
          {"session", impl_->accepted_snapshot_locked(*it->second)},
          {"events", events},
          {"nextSeq", it->second->next_seq - 1}};
}

bool TransportManager::listener_send(const std::string& id,
                                     const std::string& session_id,
                                     const std::string& peer_host, int peer_port,
                                     const std::string& payload,
                                     const std::string& encoding,
                                     std::string* error) {
  std::shared_ptr<Impl::Listener> isolated;
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    auto listener = impl_->listener_locked(id);
    if (!listener) {
      if (error) *error = "listener not found";
      return false;
    }
    if (listener->config.isolated) isolated = listener;
  }
  if (isolated) {
    if (!isolated->child || !isolated->child->running()) {
      if (error) *error = "listener is not running";
      return false;
    }
    return isolated->child->send(session_id, peer_host, peer_port, payload,
                                 encoding, error);
  }
  std::string bytes;
  if (!decode_payload(payload, encoding, &bytes, error)) return false;
  if (bytes.empty()) {
    if (error) *error = "payload is empty";
    return false;
  }

  bool tcp_listener = false;
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    auto listener = impl_->listener_locked(id);
    if (!listener) {
      if (error) *error = "listener not found";
      return false;
    }
    if (listener->state != "listening") {
      if (error) *error = "listener is not running";
      return false;
    }
    tcp_listener = listener->config.transport == "tcp";
    if (tcp_listener) {
      auto it = listener->sessions.find(session_id);
      if (it == listener->sessions.end() || it->second->client == nullptr ||
          it->second->state != "connected") {
        if (error) *error = "session is not connected";
        return false;
      }
    } else {
      if (peer_host.empty() || peer_port <= 0 || peer_port > 65535) {
        if (error) *error = "peer host and port are required";
        return false;
      }
      struct sockaddr_in addr4;
      struct sockaddr_in6 addr6;
      if (uv_ip4_addr(peer_host.c_str(), peer_port, &addr4) != 0 &&
          uv_ip6_addr(peer_host.c_str(), peer_port, &addr6) != 0) {
        if (error) *error = "peer address must be a numeric IP";
        return false;
      }
    }
  }

  if (!impl_->post([this, id, session_id, peer_host, peer_port, bytes, tcp_listener] {
        if (tcp_listener) {
          impl_->enqueue_accepted_write(id, session_id, bytes);
        } else {
          std::string ignored;
          impl_->send_udp(id, peer_host, peer_port, bytes, &ignored);
        }
      })) {
    if (error) *error = "transport manager is stopping";
    return false;
  }
  return true;
}

bool TransportManager::listener_close_session(const std::string& id,
                                              const std::string& session_id,
                                              std::string* error) {
  std::shared_ptr<Impl::Listener> isolated;
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    auto listener = impl_->listener_locked(id);
    if (!listener) {
      if (error) *error = "listener not found";
      return false;
    }
    if (listener->config.isolated) isolated = listener;
  }
  if (isolated) {
    if (!isolated->child || !isolated->child->running()) {
      if (error) *error = "listener is not running";
      return false;
    }
    return isolated->child->close_session(session_id, error);
  }
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    auto listener = impl_->listener_locked(id);
    if (!listener) {
      if (error) *error = "listener not found";
      return false;
    }
    if (listener->config.transport != "tcp") {
      if (error) *error = "UDP listeners do not have sessions";
      return false;
    }
    auto it = listener->sessions.find(session_id);
    if (it == listener->sessions.end()) {
      if (error) *error = "session not found";
      return false;
    }
  }
  if (!impl_->post([this, id, session_id] {
        impl_->close_accepted(id, session_id);
      })) {
    if (error) *error = "transport manager is stopping";
    return false;
  }
  return true;
}

}  // namespace uvscope
