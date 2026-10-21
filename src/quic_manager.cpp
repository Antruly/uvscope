#include "quic_manager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include <uv.h>
#include <uvcpp/uvcpp_config.h>

#include "util.h"

#if UVCPP_QUIC_ENABLE
#include <quic/uvcpp_quic_client.h>
#include <quic/uvcpp_quic_common.h>
#include <quic/uvcpp_quic_connection.h>
#include <quic/uvcpp_quic_server.h>
#include <ssl/uvcpp_ssl_context.h>
#endif

namespace uvscope {
namespace {

constexpr std::size_t kMaxReplyBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaxEvents = 1000;

#if UVCPP_QUIC_ENABLE
std::string read_text(const uvcpp::net_read_result& result) {
  if (!result.is_data() || result.data == nullptr) return {};
  return std::string(result.data, result.size);
}
#endif

}  // namespace

struct QuicManager::Impl {
  struct Server {
    QuicServerConfig config;
    mutable std::mutex mutex;
    std::string state = "stopped";
    std::string error;
    std::string started_at;
    std::string stopped_at;
    int bound_port = 0;
    std::atomic<std::uint64_t> connections{0};
    std::atomic<std::uint64_t> streams{0};
    std::atomic<std::uint64_t> datagrams{0};
    std::atomic<std::uint64_t> bytes{0};
    std::uint64_t next_seq = 1;
    std::deque<nlohmann::json> events;
    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<uv_async_t*> wake{nullptr};

    void add_event(nlohmann::json event) {
      std::lock_guard<std::mutex> lock(mutex);
      event["seq"] = next_seq++;
      event["at"] = now_iso8601();
      events.push_back(std::move(event));
      while (events.size() > kMaxEvents) events.pop_front();
    }
  };

  mutable std::mutex map_mutex;
  std::map<std::string, std::shared_ptr<Server>> servers;
#if UVCPP_QUIC_ENABLE
  bool crypto_ready = false;
#endif
};

QuicManager::QuicManager() : impl_(new Impl()) {
#if UVCPP_QUIC_ENABLE
  impl_->crypto_ready = uvcpp::quic_crypto_backend_init() == 0;
#endif
}

QuicManager::~QuicManager() {
  std::vector<std::shared_ptr<Impl::Server>> servers;
  {
    std::lock_guard<std::mutex> lock(impl_->map_mutex);
    for (auto& item : impl_->servers) servers.push_back(item.second);
  }
  for (auto& server : servers) {
    server->stop.store(true);
    if (uv_async_t* wake = server->wake.load()) uv_async_send(wake);
  }
  for (auto& server : servers) {
    if (server->thread.joinable()) server->thread.join();
  }
#if UVCPP_QUIC_ENABLE
  if (impl_->crypto_ready) uvcpp::quic_crypto_backend_free();
#endif
}

bool QuicManager::compiled() {
#if UVCPP_QUIC_ENABLE
  return true;
#else
  return false;
#endif
}

std::string QuicManager::unavailable_reason() {
#if UVCPP_QUIC_ENABLE
  return {};
#else
  return "QUIC is experimental and this build has no QUIC module. "
         "Build libuvcpp with UVCPP_ENABLE_QUIC=ON and an OpenSSL 3.2+ SDK.";
#endif
}

nlohmann::json QuicManager::probe(const QuicProbeInput& input) const {
  nlohmann::json result;
  result["ok"] = false;
  result["host"] = input.host;
  result["port"] = input.port;
  result["alpn"] = input.alpn;
#if !UVCPP_QUIC_ENABLE
  (void)input;
  result["error"] = unavailable_reason();
  result["code"] = "unsupported";
  return result;
#else
  if (!impl_->crypto_ready) {
    result["error"] = "QUIC crypto backend is unavailable";
    result["code"] = "unsupported";
    return result;
  }
  struct State {
    bool got_alpn = false;
    bool finished = false;
    bool timed_out = false;
    int error = 0;
    int close_code = 0;
    int64_t stream_id = -1;
    std::string alpn;
    std::string reply;
    std::string close_reason;
  } state;

  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_quic_client client(&loop);
  uvcpp::uvcpp_ssl_context ssl(uvcpp::tls_mode::CLIENT, uvcpp::tls_version::TLS_1_3);
  if (input.verify) {
    const std::string ca = find_ca_bundle(executable_dir(nullptr));
    if (!ca.empty()) ssl.load_ca_file(ca);
    ssl.set_verify_mode(uvcpp::tls_verify_mode::PEER_STRICT);
  } else {
    ssl.set_verify_mode(uvcpp::tls_verify_mode::NONE);
  }
  client.set_ssl_context(&ssl);
  client.set_alpn_protos({input.alpn});
  const int timeout_ms = std::max(1000, std::min(input.timeout_ms, 120000));
  client.set_idle_timeout(static_cast<std::uint64_t>(timeout_ms));

  const auto started = std::chrono::steady_clock::now();
  const int connect_rc = client.connect(
      input.host.c_str(), input.port, [&state, &client, &loop](int status) {
        if (status != 0) {
          state.error = status;
          state.finished = true;
          client.close();
          loop.stop();
        }
      });
  if (connect_rc != 0) {
    result["error"] = "could not start QUIC connection: " + uv_error_string(connect_rc);
    result["code"] = "connect_failed";
    return result;
  }
  auto* conn = client.connection();
  if (conn == nullptr) {
    result["error"] = "QUIC connection object is unavailable";
    result["code"] = "connect_failed";
    return result;
  }
  uvcpp::uvcpp_quic_connection::callbacks callbacks;
  callbacks.on_alpn = [&state, &conn, &input](uvcpp::uvcpp_quic_connection&,
                                              const std::string& alpn) {
    state.got_alpn = true;
    state.alpn = alpn;
    state.stream_id = conn->open_stream(true);
    if (state.stream_id < 0) {
      state.error = static_cast<int>(state.stream_id);
      state.finished = true;
      conn->close(0);
      return;
    }
    conn->write_stream(state.stream_id, input.payload.data(), input.payload.size(),
                       true);
  };
  callbacks.on_read = [&state](uvcpp::uvcpp_quic_connection& conn,
                               int64_t stream_id, const uvcpp::net_read_result& read) {
    if (stream_id != state.stream_id) return;
    if (read.is_data() && read.data != nullptr) {
      if (state.reply.size() < kMaxReplyBytes) {
        const std::size_t room = kMaxReplyBytes - state.reply.size();
        state.reply.append(read.data, std::min(room, read.size));
      }
    }
    if (read.is_end()) {
      state.finished = true;
      conn.close(0);
    }
  };
  callbacks.on_close = [&state](uvcpp::uvcpp_quic_connection&, int code) {
    state.close_code = code;
    state.finished = true;
  };
  conn->set_callbacks(callbacks);
  loop.run(UV_RUN_DEFAULT);

  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  result["timing"] = {{"totalMs", elapsed}};
  if (state.error != 0) {
    result["error"] = "QUIC request failed: " + uv_error_string(state.error);
    result["code"] = "request_failed";
    return result;
  }
  if (state.reply.empty() && state.close_code == 0 && !state.got_alpn) {
    result["error"] = "QUIC handshake did not complete";
    result["code"] = "connect_failed";
    return result;
  }
  if (state.reply.empty()) {
    result["error"] = state.close_code != 0
                          ? "connection closed before a reply: " +
                                (state.close_code > 0
                                     ? std::to_string(state.close_code)
                                     : uvcpp::quic_error_string(state.close_code))
                          : "no reply before the idle timeout";
    result["code"] = "timeout";
    return result;
  }
  result["ok"] = true;
  result["streamId"] = state.stream_id;
  result["negotiatedAlpn"] = state.alpn;
  result["replySize"] = state.reply.size();
  const bool text = std::all_of(state.reply.begin(), state.reply.end(),
                                [](unsigned char c) { return c >= 0x20 || c == '\n' || c == '\r' || c == '\t'; });
  result["replyEncoding"] = text ? "utf8" : "base64";
  result["replyText"] = text ? state.reply : std::string();
  result["replyBase64"] = text ? std::string() : base64_encode(state.reply);
  return result;
#endif
}

namespace {

#if UVCPP_QUIC_ENABLE
void run_quic_server(std::shared_ptr<QuicManager::Impl::Server> server) {
  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_ssl_context ssl(uvcpp::tls_mode::SERVER, uvcpp::tls_version::TLS_1_3);
  bool tls_ok = false;
  if (!server->config.cert_path.empty() && !server->config.key_path.empty()) {
    tls_ok = ssl.load_certificate_file(server->config.cert_path) &&
             ssl.load_private_key_file(server->config.key_path);
  } else {
    tls_ok = ssl.generate_self_signed("localhost", 2048);
  }
  if (!tls_ok) {
    std::lock_guard<std::mutex> lock(server->mutex);
    server->state = "error";
    server->error = "could not load or generate the QUIC server certificate";
    return;
  }
  uvcpp::uvcpp_quic_server quic(&loop);
  quic.set_ssl_context(&ssl);
  quic.set_alpn_select_protos({server->config.alpn});
  const int bind_rc = quic.bind(server->config.host.c_str(), server->config.port);
  if (bind_rc != 0) {
    std::lock_guard<std::mutex> lock(server->mutex);
    server->state = "error";
    server->error = "bind failed: " + uv_error_string(bind_rc);
    return;
  }
  auto* wake = new uv_async_t();
  uv_loop_t* raw_loop = reinterpret_cast<uv_loop_t*>(loop.get_handle());
  if (uv_async_init(raw_loop, wake, [](uv_async_t* handle) {
        auto* srv = static_cast<uvcpp::uvcpp_quic_server*>(handle->data);
        if (srv != nullptr) srv->stop();
      }) != 0) {
    delete wake;
    std::lock_guard<std::mutex> lock(server->mutex);
    server->state = "error";
    server->error = "could not start the QUIC server loop";
    return;
  }
  wake->data = &quic;
  server->wake.store(wake);

  const int listen_rc = quic.listen([server](uvcpp::uvcpp_quic_connection* conn) {
    const std::uint64_t connection = ++server->connections;
    (void)connection;
    server->add_event({{"type", "connection"}, {"connection", connection}});
    uvcpp::uvcpp_quic_connection::callbacks callbacks;
    callbacks.on_alpn = [server, connection](uvcpp::uvcpp_quic_connection&,
                                             const std::string& alpn) {
      server->add_event(
          {{"type", "alpn"}, {"connection", connection}, {"alpn", alpn}});
    };
    callbacks.on_stream_open = [server, connection](
                                   uvcpp::uvcpp_quic_connection&,
                                   int64_t stream_id) {
      ++server->streams;
      server->add_event({{"type", "stream-open"},
                         {"connection", connection},
                         {"streamId", stream_id}});
    };
    callbacks.on_read = [server, connection](uvcpp::uvcpp_quic_connection& conn,
                                             int64_t stream_id,
                                             const uvcpp::net_read_result& read) {
      if (read.is_data() && read.data != nullptr) {
        server->bytes += read.size;
        server->add_event({{"type", "data"},
                           {"connection", connection},
                           {"streamId", stream_id},
                           {"size", read.size},
                           {"fin", read.fin},
                           {"base64", base64_encode(
                                          std::string(read.data, read.size))}});
        if (server->config.echo) {
          conn.write_stream(stream_id, read.data, read.size, read.fin);
        }
      } else if (read.is_end()) {
        server->add_event({{"type", "stream-end"},
                           {"connection", connection},
                           {"streamId", stream_id},
                           {"fin", read.fin},
                           {"error", read.error}});
      }
    };
    callbacks.on_datagram = [server, connection](
                                uvcpp::uvcpp_quic_connection& conn,
                                const std::uint8_t* data, std::size_t len) {
      ++server->datagrams;
      server->add_event({{"type", "datagram"},
                         {"connection", connection},
                         {"size", len},
                         {"base64",
                          base64_encode(std::string(reinterpret_cast<const char*>(data), len))}});
      if (server->config.echo && len > 0) {
        conn.send_datagram(reinterpret_cast<const char*>(data), len);
      }
    };
    callbacks.on_close = [server, connection](uvcpp::uvcpp_quic_connection&,
                                              int code) {
      server->add_event(
          {{"type", "connection-close"}, {"connection", connection}, {"code", code}});
    };
    conn->set_callbacks(callbacks);
  });
  if (listen_rc != 0) {
    uv_close(reinterpret_cast<uv_handle_t*>(wake), [](uv_handle_t* handle) {
      delete reinterpret_cast<uv_async_t*>(handle);
    });
    server->wake.store(nullptr);
    std::lock_guard<std::mutex> lock(server->mutex);
    server->state = "error";
    server->error = "listen failed: " + uv_error_string(listen_rc);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(server->mutex);
    server->state = "listening";
    server->error.clear();
    server->bound_port = quic.configured_port();
    server->started_at = now_iso8601();
  }
  server->add_event({{"type", "started"},
                     {"host", server->config.host},
                     {"port", server->bound_port}});
  quic.run(UV_RUN_DEFAULT);
  if (wake != nullptr) {
    uv_close(reinterpret_cast<uv_handle_t*>(wake), [](uv_handle_t* handle) {
      delete reinterpret_cast<uv_async_t*>(handle);
    });
    loop.run(UV_RUN_NOWAIT);
  }
  server->wake.store(nullptr);
  {
    std::lock_guard<std::mutex> lock(server->mutex);
    if (server->state != "error") server->state = "stopped";
    server->stopped_at = now_iso8601();
  }
  server->add_event({{"type", "stopped"}});
}
#endif

}  // namespace

nlohmann::json QuicManager::add_server(const QuicServerConfig& config,
                                       std::string* error) {
#if !UVCPP_QUIC_ENABLE
  (void)config;
  if (error) *error = unavailable_reason();
  return nullptr;
#else
  auto server = std::make_shared<Impl::Server>();
  server->config = config;
  server->config.id = config.id.empty() ? random_hex(12) : config.id;
  if (server->config.name.empty()) {
    server->config.name = "QUIC " + std::to_string(server->config.port);
  }
  server->config.alpn = server->config.alpn.empty() ? "h3" : server->config.alpn;
  std::lock_guard<std::mutex> lock(impl_->map_mutex);
  impl_->servers[server->config.id] = server;
  return {{"id", server->config.id},
          {"name", server->config.name},
          {"host", server->config.host},
          {"port", server->config.port},
          {"alpn", server->config.alpn},
          {"state", server->state}};
#endif
}

bool QuicManager::start_server(const std::string& id, std::string* error) {
#if !UVCPP_QUIC_ENABLE
  (void)id;
  if (error) *error = unavailable_reason();
  return false;
#else
  std::shared_ptr<Impl::Server> server;
  {
    std::lock_guard<std::mutex> lock(impl_->map_mutex);
    const auto it = impl_->servers.find(id);
    if (it == impl_->servers.end()) {
      if (error) *error = "QUIC server not found";
      return false;
    }
    server = it->second;
  }
  if (server->thread.joinable()) server->thread.join();
  server->stop.store(false);
  server->thread = std::thread(run_quic_server, server);
  for (int i = 0; i < 50; ++i) {
    {
      std::lock_guard<std::mutex> lock(server->mutex);
      if (server->state == "listening" || server->state == "error") {
        if (server->state == "error" && error) *error = server->error;
        return server->state == "listening";
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (error) *error = "QUIC server did not start";
  return false;
#endif
}

bool QuicManager::stop_server(const std::string& id, std::string* error) {
#if !UVCPP_QUIC_ENABLE
  (void)id;
  if (error) *error = unavailable_reason();
  return false;
#else
  std::shared_ptr<Impl::Server> server;
  {
    std::lock_guard<std::mutex> lock(impl_->map_mutex);
    const auto it = impl_->servers.find(id);
    if (it == impl_->servers.end()) {
      if (error) *error = "QUIC server not found";
      return false;
    }
    server = it->second;
  }
  server->stop.store(true);
  if (uv_async_t* wake = server->wake.load()) uv_async_send(wake);
  if (server->thread.joinable()) server->thread.join();
  return true;
#endif
}

bool QuicManager::remove_server(const std::string& id, std::string* error) {
  if (!stop_server(id, error)) return false;
  std::lock_guard<std::mutex> lock(impl_->map_mutex);
  return impl_->servers.erase(id) != 0;
}

nlohmann::json QuicManager::servers() const {
  nlohmann::json out = nlohmann::json::array();
  std::lock_guard<std::mutex> lock(impl_->map_mutex);
  for (const auto& item : impl_->servers) {
    const auto& server = item.second;
    std::lock_guard<std::mutex> item_lock(server->mutex);
    out.push_back({{"id", server->config.id},
                   {"name", server->config.name},
                   {"host", server->config.host},
                   {"port", server->config.port},
                   {"alpn", server->config.alpn},
                   {"state", server->state},
                   {"error", server->error},
                   {"boundPort", server->bound_port},
                   {"connections", server->connections.load()},
                   {"streams", server->streams.load()},
                   {"datagrams", server->datagrams.load()},
                   {"bytes", server->bytes.load()},
                   {"startedAt", server->started_at},
                   {"stoppedAt", server->stopped_at},
                   {"nextSeq", server->next_seq - 1}});
  }
  return {{"ok", true}, {"servers", out}};
}

nlohmann::json QuicManager::server_events(const std::string& id,
                                          std::uint64_t after, int limit) const {
  nlohmann::json events = nlohmann::json::array();
  std::lock_guard<std::mutex> lock(impl_->map_mutex);
  const auto it = impl_->servers.find(id);
  if (it == impl_->servers.end()) return nullptr;
  const auto& server = it->second;
  std::lock_guard<std::mutex> item_lock(server->mutex);
  const int cap = limit <= 0 || limit > 500 ? 200 : limit;
  std::deque<nlohmann::json> selected;
  for (const auto& event : server->events) {
    if (event.value("seq", std::uint64_t{0}) <= after) continue;
    selected.push_back(event);
  }
  while (selected.size() > static_cast<std::size_t>(cap)) selected.pop_front();
  for (const auto& event : selected) events.push_back(event);
  return {{"ok", true}, {"events", events}};
}

}  // namespace uvscope
