#include "transport_manager.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
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

  mutable std::mutex state_mutex;
  std::map<std::string, std::shared_ptr<Session>> sessions;

  // Loop-thread only.
  std::map<std::string, std::unique_ptr<Active>> active;
  std::vector<uvcpp::uvcpp_tcp_client*> retired;

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
            {"queuedBytes", session.queued_bytes}};
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
                     int timeout_ms) {
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

    const int rc = client->connect(host.c_str(), port, [this, id](int status) {
      if (status != 0) {
        bool closing = false;
        {
          std::lock_guard<std::mutex> lock(state_mutex);
          auto session = session_locked(id);
          closing = session && session->state == "closing";
        }
        if (!closing) {
          mark_error(id, "connect failed: " + uv_error_string(status));
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
      mark_error(id, "connect could not start: " + uv_error_string(rc));
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

  void sweep() {
    for (uvcpp::uvcpp_tcp_client* client : retired) {
      if (client != nullptr) delete client;
    }
    retired.clear();

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

TransportManager::TransportManager() : impl_(new Impl()) {
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
                                                    std::string* error) {
  if (!available()) {
    if (error) *error = start_error().empty() ? "transport manager is not running" : start_error();
    return {{"ok", false}, {"error", error ? *error : "transport manager is not running"}};
  }
  if (host.empty() || port <= 0 || port > 65535) {
    if (error) *error = "host and port are required";
    return {{"ok", false}, {"error", "host and port are required"}};
  }
  const int timeout = std::max(500, std::min(connect_timeout_ms, 120000));

  auto session = std::make_shared<Impl::Session>();
  session->id = make_id();
  session->host = host;
  session->port = port;
  session->connect_timeout_ms = timeout;
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
  if (!impl_->post([this, id, host, port, timeout] {
        impl_->start_connect(id, host, port, timeout);
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

}  // namespace uvscope
