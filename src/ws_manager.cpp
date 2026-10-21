#include "ws_manager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include <uv.h>
#include <ssl/uvcpp_ssl_context.h>
#include <web/uvcpp_ws_client.h>
#include <web/uvcpp_ws_frame.h>

#include "util.h"

namespace uvscope {
namespace {

constexpr std::size_t kMaxSessionsPerUser = 8;
constexpr std::size_t kMaxSessions = 16;
constexpr std::size_t kMaxEvents = 1000;
constexpr std::size_t kMaxFramePayload = 16 * 1024;
constexpr std::size_t kMaxMessagePayload = 256 * 1024;
constexpr std::size_t kMaxSendBytes = 8 * 1024 * 1024;
constexpr int kConnectTimeoutMs = 15000;

constexpr const char* kOpcodeNames[] = {"CONTINUATION", "TEXT", "BINARY",
                                        "RESERVED3",    "RESERVED4", "RESERVED5",
                                        "RESERVED6",    "RESERVED7", "CLOSE",
                                        "PING",         "PONG"};

std::string opcode_name(uvcpp::ws_opcode opcode) {
  const auto index = static_cast<std::size_t>(opcode);
  return index < sizeof(kOpcodeNames) / sizeof(kOpcodeNames[0])
             ? kOpcodeNames[index]
             : "UNKNOWN";
}

bool valid_utf8(const std::string& text) {
  std::size_t i = 0;
  while (i < text.size()) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    std::size_t extra = 0;
    if (c < 0x80) {
      ++i;
      continue;
    }
    if ((c & 0xE0) == 0xC0) extra = 1;
    else if ((c & 0xF0) == 0xE0) extra = 2;
    else if ((c & 0xF8) == 0xF0) extra = 3;
    else return false;
    if (i + extra >= text.size()) return false;
    for (std::size_t j = 1; j <= extra; ++j) {
      if ((static_cast<unsigned char>(text[i + j]) & 0xC0) != 0x80) return false;
    }
    i += extra + 1;
  }
  return true;
}

std::string payload_base64(const std::string& data, std::size_t cap, bool* truncated) {
  const std::size_t take = std::min(data.size(), cap);
  if (truncated) *truncated = take < data.size();
  return base64_encode(data.substr(0, take));
}

bool is_ws_url(const std::string& url) {
  const std::string lower_url = lower(trim(url));
  return lower_url.rfind("ws://", 0) == 0 || lower_url.rfind("wss://", 0) == 0;
}

}  // namespace

struct WsManager::Impl {
  struct Command {
    enum Kind { SendText, SendBinary, SendPing, SendPong, Close, Stop } kind =
        SendText;
    std::string payload;
    int code = 1000;
    std::string reason;
  };

  struct Session {
    std::string id;
    std::string owner_name;
    std::string url;
    std::int64_t owner_id = 0;
    std::string state = "connecting";  // connecting|open|closing|closed|error
    std::string error;
    std::string created_at;
    std::string connected_at;
    std::string closed_at;
    std::uint64_t next_seq = 1;
    std::uint64_t sent_bytes = 0;
    std::uint64_t received_bytes = 0;
    std::uint64_t frame_count = 0;
    int close_code = 0;
    std::string close_reason;
    std::chrono::steady_clock::time_point closing_since{};
    bool remove_requested = false;
    std::chrono::steady_clock::time_point finished_wall;

    mutable std::mutex mutex;
    std::deque<nlohmann::json> events;

    std::mutex command_mutex;
    std::deque<Command> commands;
    std::atomic<uv_async_t*> wake{nullptr};
    uv_timer_t* timer = nullptr;
    std::thread thread;
  };

  struct WakeContext {
    std::shared_ptr<Session> session;
    uvcpp::uvcpp_ws_client* client = nullptr;
    std::chrono::steady_clock::time_point connect_deadline{};
  };

  mutable std::mutex map_mutex;
  std::map<std::string, std::shared_ptr<Session>> sessions;
  bool available = true;
  std::string start_error;

  std::shared_ptr<Session> find_locked(const std::string& id) const {
    const auto it = sessions.find(id);
    return it == sessions.end() ? nullptr : it->second;
  }

  static void add_event(const std::shared_ptr<Session>& session,
                        nlohmann::json event) {
    std::lock_guard<std::mutex> lock(session->mutex);
    event["seq"] = session->next_seq++;
    event["at"] = now_iso8601();
    session->events.push_back(std::move(event));
    while (session->events.size() > kMaxEvents) session->events.pop_front();
  }

  static void set_state(const std::shared_ptr<Session>& session,
                        const std::string& state, const std::string& error) {
    std::lock_guard<std::mutex> lock(session->mutex);
    session->state = state;
    if (!error.empty()) session->error = error;
    if (state != "connecting" && state != "open" && state != "closing") {
      session->closed_at = now_iso8601();
      session->finished_wall = std::chrono::steady_clock::now();
    }
  }

  static bool is_terminal(const std::string& state) {
    return state == "closed" || state == "error";
  }

  static void close_handles(const std::shared_ptr<Session>& session) {
    uv_async_t* wake = nullptr;
    uv_timer_t* timer = nullptr;
    {
      std::lock_guard<std::mutex> lock(session->mutex);
      wake = session->wake.exchange(nullptr);
      timer = session->timer;
      session->timer = nullptr;
    }
    if (timer != nullptr) {
      uv_timer_stop(timer);
      uv_close(reinterpret_cast<uv_handle_t*>(timer), [](uv_handle_t* handle) {
        delete static_cast<WakeContext*>(handle->data);
        delete reinterpret_cast<uv_timer_t*>(handle);
      });
    }
    if (wake != nullptr) {
      uv_close(reinterpret_cast<uv_handle_t*>(wake), [](uv_handle_t* handle) {
        delete static_cast<WakeContext*>(handle->data);
        delete reinterpret_cast<uv_async_t*>(handle);
      });
    }
  }

  static nlohmann::json snapshot_locked(const Session& session) {
    return {{"id", session.id},
            {"ownerId", session.owner_id},
            {"owner", session.owner_name},
            {"url", session.url},
            {"state", session.state},
            {"error", session.error},
            {"createdAt", session.created_at},
            {"connectedAt", session.connected_at},
            {"closedAt", session.closed_at},
            {"sentBytes", session.sent_bytes},
            {"receivedBytes", session.received_bytes},
            {"frameCount", session.frame_count},
            {"closeCode", session.close_code},
            {"closeReason", session.close_reason},
            {"nextSeq", session.next_seq - 1}};
  }

  static void apply_command(const std::shared_ptr<Session>& session,
                            uvcpp::uvcpp_ws_client& client,
                            const Command& command) {
    if (command.kind == Command::Stop) {
      {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->remove_requested = true;
      }
      if (client.session() == nullptr) {
        set_state(session, "closed", "");
        client.stop();
        return;
      }
      set_state(session, "closing", "");
      {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->close_code = 1001;
        session->close_reason = "session deleted";
        session->closing_since = std::chrono::steady_clock::now();
      }
      client.close(uvcpp::ws_close_code::GOING_AWAY, "session deleted");
      return;
    }
    if (command.kind == Command::Close) {
      if (client.session() == nullptr) {
        add_event(session, {{"type", "error"},
                            {"error", "session is not open"},
                            {"detail", "close dropped"},
                            {"closeCode", command.code}});
        return;
      }
      set_state(session, "closing", "");
      {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->close_code = command.code;
        session->close_reason = command.reason;
        session->closing_since = std::chrono::steady_clock::now();
      }
      add_event(session, {{"type", "closing"},
                          {"code", command.code},
                          {"reason", command.reason}});
      client.close(static_cast<uvcpp::ws_close_code>(command.code), command.reason);
      return;
    }
    if (client.session() == nullptr) {
      add_event(session, {{"type", "error"},
                          {"error", "session is not open"},
                          {"detail", "send dropped"}});
      return;
    }
    if (command.payload.size() > kMaxSendBytes) {
      add_event(session, {{"type", "error"},
                          {"error", "send payload exceeds 8 MiB"}});
      return;
    }
    int rc = 0;
    const char* label = "text";
    if (command.kind == Command::SendText) {
      rc = client.send_text(command.payload.data(), command.payload.size());
    } else if (command.kind == Command::SendBinary) {
      label = "binary";
      rc = client.send_binary(command.payload.data(), command.payload.size());
    } else {
      uvcpp::uvcpp_ws_frame frame;
      frame.fin = true;
      frame.opcode = command.kind == Command::SendPing ? uvcpp::ws_opcode::PING
                                                       : uvcpp::ws_opcode::PONG;
      frame.payload.clone_data(command.payload.data(), command.payload.size());
      label = command.kind == Command::SendPing ? "ping" : "pong";
      rc = client.send_frame(frame);
    }
    if (rc != 0) {
      add_event(session, {{"type", "error"},
                          {"error", "send failed: " + uv_error_string(rc)}});
      return;
    }
    {
      std::lock_guard<std::mutex> lock(session->mutex);
      session->sent_bytes += command.payload.size();
    }
    add_event(session, {{"type", "sent"},
                        {"opcode", label},
                        {"size", command.payload.size()}});
  }

  static void drain_commands(const std::shared_ptr<Session>& session,
                             uvcpp::uvcpp_ws_client& client) {
    std::deque<Command> pending;
    {
      std::lock_guard<std::mutex> lock(session->command_mutex);
      pending.swap(session->commands);
    }
    for (const auto& command : pending) apply_command(session, client, command);
  }


};

namespace {
void wake_callback(uv_async_t* handle) {
  auto* context = static_cast<WsManager::Impl::WakeContext*>(handle->data);
  if (context == nullptr || context->client == nullptr) return;
  WsManager::Impl::drain_commands(context->session, *context->client);
}

void timeout_callback(uv_timer_t* timer) {
  auto* context = static_cast<WsManager::Impl::WakeContext*>(timer->data);
  if (context == nullptr || context->client == nullptr) return;
  const auto now = std::chrono::steady_clock::now();
  std::string state;
  int close_code = 0;
  std::string close_reason;
  std::chrono::steady_clock::time_point closing_since{};
  {
    std::lock_guard<std::mutex> lock(context->session->mutex);
    state = context->session->state;
    close_code = context->session->close_code;
    close_reason = context->session->close_reason;
    closing_since = context->session->closing_since;
  }
  if (state == "connecting") {
    if (now < context->connect_deadline) return;
    WsManager::Impl::set_state(context->session, "error", "connect timed out");
    WsManager::Impl::add_event(context->session,
                               {{"type", "error"}, {"error", "connect timed out"}});
    context->client->close(uvcpp::ws_close_code::GOING_AWAY, "connect timeout");
    context->client->stop();
    return;
  }
  // A locally initiated close does not invoke on_close (the caller already
  // knows), so watch the session until it is retired, with a bounded grace
  // period for a peer that never answers the Close frame.
  const bool closing = state == "closing";
  const bool abandoned = state == "open" && context->client->session() == nullptr;
  if (!closing && !abandoned) return;
  const bool retired = context->client->session() == nullptr;
  const bool timed_out = closing && now - closing_since > std::chrono::seconds(5);
  if (!retired && !timed_out) return;
  {
    std::lock_guard<std::mutex> lock(context->session->mutex);
    context->session->state = "closed";
    context->session->closed_at = now_iso8601();
    context->session->finished_wall = std::chrono::steady_clock::now();
    if (abandoned) {
      context->session->close_code = 1006;
      context->session->close_reason = "connection ended";
    }
  }
  WsManager::Impl::add_event(
      context->session,
      {{"type", "close"},
       {"code", abandoned ? 1006 : close_code},
       {"reason", abandoned ? std::string("connection ended") : close_reason}});
  WsManager::Impl::close_handles(context->session);
  context->client->stop();
}
}  // namespace

namespace {

void run_session(WsManager::Impl* impl,
                 std::shared_ptr<WsManager::Impl::Session> session,
                 WsConnectInput input) {
  WsManager::Impl::add_event(session, {{"type", "connecting"}, {"url", session->url}});
  // Declared before the client so the TLS context outlives the connection.
  uvcpp::uvcpp_ssl_context ssl(uvcpp::tls_mode::CLIENT, uvcpp::tls_version::TLS_1_2);
  uvcpp::uvcpp_ws_client client;
  if (lower(session->url).rfind("wss://", 0) == 0) {
    const std::string ca = find_ca_bundle(executable_dir(nullptr));
    if (!ca.empty()) ssl.load_ca_file(ca);
    ssl.set_verify_mode(input.insecure ? uvcpp::tls_verify_mode::NONE
                                       : uvcpp::tls_verify_mode::PEER_STRICT);
    client.set_ssl_context(&ssl);
  }
  if (input.proxy.enabled()) {
    std::string proxy_error;
    if (!input.proxy.valid(&proxy_error)) {
      WsManager::Impl::set_state(session, "error", proxy_error);
      WsManager::Impl::add_event(session, {{"type", "error"}, {"error", proxy_error}});
      return;
    }
    const int rc = client.set_socks5_proxy(input.proxy.host, input.proxy.port,
                                           input.proxy.username, input.proxy.password);
    if (rc != 0) {
      const std::string error = "invalid SOCKS5 proxy: " + uv_error_string(rc);
      WsManager::Impl::set_state(session, "error", error);
      WsManager::Impl::add_event(session, {{"type", "error"}, {"error", error}});
      return;
    }
  }
  for (const auto& header : input.headers) {
    if (lower(trim(header.first)).empty()) continue;
    client.set_handshake_header(header.first, header.second);
  }
  if (!input.subprotocol.empty()) {
    client.set_handshake_header("Sec-WebSocket-Protocol", input.subprotocol);
  }

  WsManager::Impl::WakeContext context;
  context.session = session;
  context.client = &client;
  uvcpp::uvcpp_loop* loop = client.get_loop();
  uv_loop_t* raw_loop =
      loop == nullptr ? nullptr : reinterpret_cast<uv_loop_t*>(loop->get_handle());
  auto* wake = new uv_async_t();
  auto* timer = new uv_timer_t();
  if (raw_loop == nullptr || uv_async_init(raw_loop, wake, wake_callback) != 0 ||
      uv_timer_init(raw_loop, timer) != 0) {
    delete wake;
    delete timer;
    const std::string error = "could not start the WebSocket session loop";
    WsManager::Impl::set_state(session, "error", error);
    WsManager::Impl::add_event(session, {{"type", "error"}, {"error", error}});
    return;
  }
  // Both handles need the same trampoline context. Give the timer its own
  // heap copy because uv_handle_t::data belongs to each handle.
  auto* timer_context = new WsManager::Impl::WakeContext(context);
  wake->data = new WsManager::Impl::WakeContext(context);
  timer->data = timer_context;
  {
    std::lock_guard<std::mutex> lock(session->mutex);
    session->timer = timer;
  }
  session->wake.store(wake);
  uv_async_send(wake);
  context.connect_deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(std::max(1000, input.timeout_ms));
  uv_timer_start(timer, timeout_callback, 100, 100);

  client.on_text([session](const std::string& text) {
    {
      std::lock_guard<std::mutex> lock(session->mutex);
      session->received_bytes += text.size();
    }
    bool truncated = false;
    const std::string clipped =
        text.size() > kMaxMessagePayload ? text.substr(0, kMaxMessagePayload) : text;
    truncated = text.size() > clipped.size();
    WsManager::Impl::add_event(session, {{"type", "text"},
                                         {"text", valid_utf8(clipped) ? clipped
                                                                      : base64_encode(clipped)},
                                         {"encoding", valid_utf8(clipped) ? "utf8" : "base64"},
                                         {"size", text.size()},
                                         {"truncated", truncated}});
  });
  client.on_binary([session](const uint8_t* data, std::size_t len) {
    {
      std::lock_guard<std::mutex> lock(session->mutex);
      session->received_bytes += len;
    }
    bool truncated = false;
    const std::string bytes(reinterpret_cast<const char*>(data), len);
    const std::string encoded = payload_base64(bytes, kMaxMessagePayload, &truncated);
    WsManager::Impl::add_event(session, {{"type", "binary"},
                                         {"base64", encoded},
                                         {"size", len},
                                         {"truncated", truncated}});
  });
  client.on_frame([session](const uvcpp::uvcpp_ws_frame& frame) {
    {
      std::lock_guard<std::mutex> lock(session->mutex);
      ++session->frame_count;
    }
    const std::string data = frame.payload.to_string();
    bool truncated = false;
    const std::string encoded = payload_base64(data, kMaxFramePayload, &truncated);
    nlohmann::json event = {{"type", "frame"},
                            {"opcode", opcode_name(frame.opcode)},
                            {"opcodeId", static_cast<int>(frame.opcode)},
                            {"fin", frame.fin},
                            {"rsv1", frame.rsv1},
                            {"masked", frame.masked},
                            {"length", frame.payload_len},
                            {"truncated", truncated}};
    if (frame.opcode == uvcpp::ws_opcode::TEXT && valid_utf8(data)) {
      event["text"] = data.size() > kMaxFramePayload ? data.substr(0, kMaxFramePayload)
                                                     : data;
    } else if (!encoded.empty()) {
      event["base64"] = encoded;
    }
    if (frame.opcode == uvcpp::ws_opcode::CLOSE) {
      event["closeCode"] = static_cast<int>(frame.get_close_code());
      event["closeReason"] = frame.get_close_reason();
    }
    WsManager::Impl::add_event(session, std::move(event));
  });
  client.on_close([session, &client](uvcpp::ws_close_code code,
                                     const std::string& reason) {
    {
      std::lock_guard<std::mutex> lock(session->mutex);
      if (session->state != "error") session->state = "closed";
      session->close_code = static_cast<int>(code);
      session->close_reason = reason;
      session->closed_at = now_iso8601();
      session->finished_wall = std::chrono::steady_clock::now();
    }
    WsManager::Impl::add_event(session, {{"type", "close"},
                                         {"code", static_cast<int>(code)},
                                         {"reason", reason}});
    WsManager::Impl::close_handles(session);
    client.stop();
  });
  client.on_error([session, &client](int status, const std::string& message) {
    const std::string error =
        message.empty() ? uv_error_string(status) : message;
    WsManager::Impl::set_state(session, "error", error);
    WsManager::Impl::add_event(session, {{"type", "error"},
                                         {"error", error},
                                         {"status", status}});
    WsManager::Impl::close_handles(session);
    client.stop();
  });

  const int connect_rc = client.connect(
      session->url, [session, &client](uvcpp::uvcpp_ws_connection*, int error) {
        if (error != 0) {
          bool removed = false;
          {
            std::lock_guard<std::mutex> lock(session->mutex);
            removed = session->remove_requested;
          }
          WsManager::Impl::set_state(
              session, removed ? "closed" : "error",
              removed ? "" : "connect failed: " + uv_error_string(error));
          WsManager::Impl::add_event(
              session, {{"type", "error"},
                        {"error", removed ? "session deleted"
                                          : "connect failed: " + uv_error_string(error)}});
          WsManager::Impl::close_handles(session);
          client.stop();
          return;
        }
        WsManager::Impl::set_state(session, "open", "");
        {
          std::lock_guard<std::mutex> lock(session->mutex);
          session->connected_at = now_iso8601();
        }
        WsManager::Impl::add_event(session, {{"type", "open"}});
        bool removed = false;
        {
          std::lock_guard<std::mutex> lock(session->mutex);
          removed = session->remove_requested;
        }
        if (removed) client.close(uvcpp::ws_close_code::GOING_AWAY, "session deleted");
      });
  if (connect_rc != 0) {
    const std::string error = "could not start connection: " + uv_error_string(connect_rc);
    WsManager::Impl::set_state(session, "error", error);
    WsManager::Impl::add_event(session, {{"type", "error"}, {"error", error}});
    WsManager::Impl::close_handles(session);
    client.run(UV_RUN_NOWAIT);
  } else {
    client.run(UV_RUN_DEFAULT);
  }

  WsManager::Impl::drain_commands(session, client);
  WsManager::Impl::close_handles(session);
  bool ended_without_close = false;
  {
    std::lock_guard<std::mutex> lock(session->mutex);
    if (!WsManager::Impl::is_terminal(session->state)) {
      session->state = "closed";
      session->closed_at = now_iso8601();
      session->finished_wall = std::chrono::steady_clock::now();
      ended_without_close = true;
    }
  }
  if (ended_without_close) {
    WsManager::Impl::add_event(
        session, {{"type", "close"}, {"code", 1006}, {"reason", "connection ended"}});
  }
  client.run(UV_RUN_NOWAIT);
  (void)impl;
}

}  // namespace

WsManager::WsManager() : impl_(new Impl()) {}
WsManager::~WsManager() {
  std::vector<std::shared_ptr<Impl::Session>> sessions;
  {
    std::lock_guard<std::mutex> lock(impl_->map_mutex);
    for (auto& item : impl_->sessions) sessions.push_back(item.second);
  }
  for (auto& session : sessions) {
    {
      std::lock_guard<std::mutex> lock(session->command_mutex);
      session->commands.push_back(Impl::Command{Impl::Command::Stop, "", 1001, ""});
    }
    if (uv_async_t* wake = session->wake.load()) uv_async_send(wake);
  }
  for (auto& session : sessions) {
    if (session->thread.joinable()) session->thread.join();
  }
}

bool WsManager::available() const { return impl_->available; }
const std::string& WsManager::start_error() const { return impl_->start_error; }

std::string WsManager::create(std::int64_t owner_id, const std::string& owner_name,
                              const WsConnectInput& input, std::string* error) {
  if (!is_ws_url(input.url)) {
    if (error) *error = "URL must start with ws:// or wss://";
    return {};
  }
  auto session = std::make_shared<Impl::Session>();
  session->id = random_hex(16);
  session->owner_id = owner_id;
  session->owner_name = owner_name;
  session->url = trim(input.url);
  session->created_at = now_iso8601();
  {
    std::lock_guard<std::mutex> lock(impl_->map_mutex);
    std::size_t total = 0;
    std::size_t mine = 0;
    for (const auto& item : impl_->sessions) {
      std::lock_guard<std::mutex> item_lock(item.second->mutex);
      if (item.second->remove_requested) continue;
      if (Impl::is_terminal(item.second->state)) continue;
      ++total;
      if (item.second->owner_id == owner_id) ++mine;
    }
    if (total >= kMaxSessions) {
      if (error) *error = "too many WebSocket sessions";
      return {};
    }
    if (mine >= kMaxSessionsPerUser) {
      if (error) *error = "too many WebSocket sessions for this user";
      return {};
    }
    impl_->sessions[session->id] = session;
  }
  session->thread = std::thread(run_session, impl_.get(), session, input);
  return session->id;
}

bool WsManager::send(const std::string& id, std::int64_t owner_id, bool is_admin,
                     const std::string& type, const std::string& payload,
                     std::string* error) {
  std::shared_ptr<Impl::Session> session;
  {
    std::lock_guard<std::mutex> lock(impl_->map_mutex);
    session = impl_->find_locked(id);
  }
  if (!session || (!is_admin && session->owner_id != owner_id)) {
    if (error) *error = "session not found";
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(session->mutex);
    if (session->state != "open") {
      if (error) *error = "session is not open";
      return false;
    }
  }
  Impl::Command command;
  const std::string kind = lower(type);
  if (kind == "binary") command.kind = Impl::Command::SendBinary;
  else if (kind == "ping") command.kind = Impl::Command::SendPing;
  else if (kind == "pong") command.kind = Impl::Command::SendPong;
  else command.kind = Impl::Command::SendText;
  command.payload = payload;
  {
    std::lock_guard<std::mutex> lock(session->command_mutex);
    session->commands.push_back(std::move(command));
  }
  if (uv_async_t* wake = session->wake.load()) uv_async_send(wake);
  return true;
}

bool WsManager::close(const std::string& id, std::int64_t owner_id, bool is_admin,
                      int code, const std::string& reason, std::string* error) {
  std::shared_ptr<Impl::Session> session;
  {
    std::lock_guard<std::mutex> lock(impl_->map_mutex);
    session = impl_->find_locked(id);
  }
  if (!session || (!is_admin && session->owner_id != owner_id)) {
    if (error) *error = "session not found";
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(session->command_mutex);
    session->commands.push_back(Impl::Command{Impl::Command::Close, "", code, reason});
  }
  if (uv_async_t* wake = session->wake.load()) uv_async_send(wake);
  return true;
}

bool WsManager::remove(const std::string& id, std::int64_t owner_id, bool is_admin,
                       std::string* error) {
  std::shared_ptr<Impl::Session> session;
  {
    std::lock_guard<std::mutex> lock(impl_->map_mutex);
    session = impl_->find_locked(id);
  }
  if (!session || (!is_admin && session->owner_id != owner_id)) {
    if (error) *error = "session not found";
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(session->mutex);
    session->remove_requested = true;
    if (session->state == "closed" || session->state == "error") {
      session->closed_at = now_iso8601();
      session->finished_wall = std::chrono::steady_clock::now();
    }
  }
  {
    std::lock_guard<std::mutex> lock(session->command_mutex);
    session->commands.push_back(Impl::Command{Impl::Command::Stop, "", 1001, ""});
  }
  if (uv_async_t* wake = session->wake.load()) uv_async_send(wake);
  return true;
}

bool WsManager::owner_of(const std::string& id, std::int64_t* owner_id) const {
  std::lock_guard<std::mutex> lock(impl_->map_mutex);
  const auto session = impl_->find_locked(id);
  if (!session) return false;
  if (owner_id) *owner_id = session->owner_id;
  return true;
}

nlohmann::json WsManager::sessions(std::int64_t owner_id, bool all) const {
  nlohmann::json out = nlohmann::json::array();
  std::lock_guard<std::mutex> lock(impl_->map_mutex);
  for (const auto& item : impl_->sessions) {
    const auto& session = item.second;
    {
      std::lock_guard<std::mutex> item_lock(session->mutex);
      if (session->remove_requested) continue;
      if (!all && session->owner_id != owner_id) continue;
      out.push_back(Impl::snapshot_locked(*session));
    }
  }
  std::sort(out.begin(), out.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
    return a.value("createdAt", std::string()) > b.value("createdAt", std::string());
  });
  return {{"ok", true}, {"sessions", out}};
}

bool WsManager::session(const std::string& id, std::int64_t owner_id, bool is_admin,
                        nlohmann::json* out) const {
  std::lock_guard<std::mutex> lock(impl_->map_mutex);
  const auto session = impl_->find_locked(id);
  if (!session) return false;
  std::lock_guard<std::mutex> item_lock(session->mutex);
  if (session->remove_requested) return false;
  if (!is_admin && session->owner_id != owner_id) return false;
  if (out) *out = {{"ok", true}, {"session", Impl::snapshot_locked(*session)}};
  return true;
}

bool WsManager::events(const std::string& id, std::int64_t owner_id, bool is_admin,
                       std::uint64_t after, std::size_t limit,
                       nlohmann::json* out) const {
  std::lock_guard<std::mutex> lock(impl_->map_mutex);
  const auto session = impl_->find_locked(id);
  if (!session) return false;
  std::lock_guard<std::mutex> item_lock(session->mutex);
  if (session->remove_requested) return false;
  if (!is_admin && session->owner_id != owner_id) return false;
  if (limit == 0 || limit > kMaxEvents) limit = kMaxEvents;
  nlohmann::json events = nlohmann::json::array();
  std::deque<nlohmann::json> selected;
  for (const auto& event : session->events) {
    if (event.value("seq", std::uint64_t{0}) <= after) continue;
    selected.push_back(event);
  }
  while (selected.size() > limit) selected.pop_front();
  for (const auto& event : selected) events.push_back(event);
  if (out) {
    *out = {{"ok", true},
            {"events", events},
            {"session", Impl::snapshot_locked(*session)}};
  }
  return true;
}

}  // namespace uvscope
