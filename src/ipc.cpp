#include "ipc.h"

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <string>
#include <utility>

#include <uv.h>

#include "util.h"

namespace uvscope {
namespace {

constexpr std::uint32_t kMaxFrameBytes = 4u * 1024u * 1024u;

std::string encode_frame(const nlohmann::json& message) {
  const std::string payload = message.dump();
  if (payload.size() > kMaxFrameBytes) return {};
  const std::uint32_t length = static_cast<std::uint32_t>(payload.size());
  std::string frame(4, '\0');
  frame[0] = static_cast<char>((length >> 24) & 0xff);
  frame[1] = static_cast<char>((length >> 16) & 0xff);
  frame[2] = static_cast<char>((length >> 8) & 0xff);
  frame[3] = static_cast<char>(length & 0xff);
  frame += payload;
  return frame;
}

}  // namespace

struct IpcChannel::Impl {
  explicit Impl(uv_loop_t* loop) : loop(loop) {}

  uv_loop_t* loop;
  uv_pipe_t listener;
  bool has_listener = false;
  bool listener_closing = false;
  uv_pipe_t stream;
  bool has_stream = false;
  bool stream_closing = false;
  uv_connect_t connect_req{};
  bool connected = false;
  bool close_notified = false;
  MessageHandler on_message;
  CloseHandler on_close;
  ReadyHandler on_ready;
  std::string read_buffer;
  std::deque<std::string> write_queue;
  std::string current_write;
  uv_write_t write_req{};
  bool writing = false;

  static void on_connection(uv_stream_t* server, int status);
  static void on_connect(uv_connect_t* req, int status);
  static void alloc_cb(uv_handle_t* handle, size_t suggested, uv_buf_t* buf);
  static void read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);
  static void write_cb(uv_write_t* req, int status);
  static void close_cb(uv_handle_t* handle);

  void notify_close(const std::string& reason) {
    if (close_notified) return;
    close_notified = true;
    if (on_close) on_close(reason);
  }

  void start_read() {
    if (has_stream && !stream_closing) {
      uv_read_start(reinterpret_cast<uv_stream_t*>(&stream), alloc_cb, read_cb);
    }
  }

  void parse_frames() {
    while (read_buffer.size() >= 4) {
      const auto* data = reinterpret_cast<const unsigned char*>(read_buffer.data());
      const std::uint32_t length = (static_cast<std::uint32_t>(data[0]) << 24) |
                                   (static_cast<std::uint32_t>(data[1]) << 16) |
                                   (static_cast<std::uint32_t>(data[2]) << 8) |
                                   static_cast<std::uint32_t>(data[3]);
      if (length == 0 || length > kMaxFrameBytes) {
        notify_close("invalid IPC frame length");
        close_stream();
        return;
      }
      if (read_buffer.size() < 4 + length) break;
      std::string payload = read_buffer.substr(4, length);
      read_buffer.erase(0, 4 + length);
      auto message = nlohmann::json::parse(payload, nullptr, false);
      if (message.is_discarded()) {
        notify_close("invalid IPC JSON");
        close_stream();
        return;
      }
      if (on_message) on_message(message);
      if (stream_closing) return;
    }
  }

  void flush() {
    if (writing || write_queue.empty() || !connected) return;
    current_write = std::move(write_queue.front());
    write_queue.pop_front();
    uv_buf_t buf = uv_buf_init(&current_write[0],
                               static_cast<unsigned int>(current_write.size()));
    write_req.data = this;
    writing = true;
    const int rc = uv_write(&write_req, reinterpret_cast<uv_stream_t*>(&stream), &buf, 1,
                            write_cb);
    if (rc != 0) {
      writing = false;
      notify_close(uv_strerror(rc));
      close_stream();
    }
  }

  void close_stream() {
    if (has_stream && !stream_closing) {
      stream_closing = true;
      connected = false;
      uv_close(reinterpret_cast<uv_handle_t*>(&stream), close_cb);
    }
  }

  void close_listener() {
    if (has_listener && !listener_closing) {
      listener_closing = true;
      uv_close(reinterpret_cast<uv_handle_t*>(&listener), close_cb);
    }
  }
};

void IpcChannel::Impl::on_connection(uv_stream_t* server, int status) {
  auto* self = static_cast<Impl*>(server->data);
  if (self == nullptr) return;
  if (status != 0) {
    self->notify_close(uv_strerror(status));
    return;
  }
  if (self->has_stream) {
    auto* extra = new uv_pipe_t;
    if (uv_pipe_init(self->loop, extra, 0) == 0 &&
        uv_accept(server, reinterpret_cast<uv_stream_t*>(extra)) == 0) {
      uv_close(reinterpret_cast<uv_handle_t*>(extra),
               [](uv_handle_t* handle) { delete reinterpret_cast<uv_pipe_t*>(handle); });
    } else {
      delete extra;
    }
    return;
  }
  if (uv_pipe_init(self->loop, &self->stream, 0) != 0) {
    self->notify_close("could not initialize IPC stream");
    return;
  }
  self->stream.data = self;
  self->has_stream = true;
  if (uv_accept(server, reinterpret_cast<uv_stream_t*>(&self->stream)) != 0) {
    self->close_stream();
    return;
  }
  self->connected = true;
  self->start_read();
  if (self->on_ready) self->on_ready();
}

void IpcChannel::Impl::on_connect(uv_connect_t* req, int status) {
  auto* self = static_cast<Impl*>(req->data);
  if (self == nullptr) return;
  if (status != 0) {
    self->notify_close(uv_strerror(status));
    self->close_stream();
    return;
  }
  self->connected = true;
  self->start_read();
  if (self->on_ready) self->on_ready();
}

void IpcChannel::Impl::alloc_cb(uv_handle_t*, size_t suggested, uv_buf_t* buf) {
  const size_t size = suggested > 0 ? suggested : 65536;
  auto* data = new char[size];
  *buf = uv_buf_init(data, static_cast<unsigned int>(size));
}

void IpcChannel::Impl::read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
  auto* self = static_cast<Impl*>(stream->data);
  if (self != nullptr) {
    if (nread > 0) {
      self->read_buffer.append(buf->base, static_cast<std::size_t>(nread));
      self->parse_frames();
    } else if (nread < 0) {
      self->notify_close(uv_strerror(static_cast<int>(nread)));
      self->close_stream();
    }
  }
  delete[] buf->base;
}

void IpcChannel::Impl::write_cb(uv_write_t* req, int status) {
  auto* self = static_cast<Impl*>(req->data);
  if (self == nullptr) return;
  self->writing = false;
  self->current_write.clear();
  if (status != 0) {
    self->notify_close(uv_strerror(status));
    self->close_stream();
    return;
  }
  self->flush();
}

void IpcChannel::Impl::close_cb(uv_handle_t* handle) {
  auto* self = static_cast<Impl*>(handle->data);
  if (self == nullptr) return;
  if (handle == reinterpret_cast<uv_handle_t*>(&self->stream)) {
    self->has_stream = false;
    self->notify_close("IPC stream closed");
  } else if (handle == reinterpret_cast<uv_handle_t*>(&self->listener)) {
    self->has_listener = false;
  }
}

IpcChannel::IpcChannel(uv_loop_s* loop)
    : impl_(new Impl(reinterpret_cast<uv_loop_t*>(loop))) {}

IpcChannel::~IpcChannel() {
  impl_->close_listener();
  impl_->close_stream();
}

bool IpcChannel::listen(const std::string& path, std::string* error) {
  if (impl_->has_listener) {
    if (error) *error = "IPC channel is already listening";
    return false;
  }
  if (uv_pipe_init(impl_->loop, &impl_->listener, 0) != 0) {
    if (error) *error = "could not initialize the IPC listener";
    return false;
  }
  impl_->listener.data = impl_.get();
  const int bind_rc = uv_pipe_bind(&impl_->listener, path.c_str());
  if (bind_rc != 0) {
    impl_->has_listener = true;
    impl_->close_listener();
    if (error) *error = std::string("could not bind the IPC endpoint: ") +
                        uv_strerror(bind_rc);
    return false;
  }
  const int listen_rc =
      uv_listen(reinterpret_cast<uv_stream_t*>(&impl_->listener), 16,
                Impl::on_connection);
  if (listen_rc != 0) {
    impl_->has_listener = true;
    impl_->close_listener();
    if (error) *error = std::string("could not listen on the IPC endpoint: ") +
                        uv_strerror(listen_rc);
    return false;
  }
  impl_->has_listener = true;
  return true;
}

bool IpcChannel::connect(const std::string& path, std::string* error) {
  if (impl_->has_stream) {
    if (error) *error = "IPC channel is already connected";
    return false;
  }
  if (uv_pipe_init(impl_->loop, &impl_->stream, 0) != 0) {
    if (error) *error = "could not initialize the IPC client";
    return false;
  }
  impl_->stream.data = impl_.get();
  impl_->has_stream = true;
  impl_->connect_req.data = impl_.get();
  uv_pipe_connect(&impl_->connect_req, &impl_->stream, path.c_str(), Impl::on_connect);
  return true;
}

bool IpcChannel::send(const nlohmann::json& message, std::string* error) {
  if (!impl_->connected) {
    if (error) *error = "IPC channel is not connected";
    return false;
  }
  std::string frame = encode_frame(message);
  if (frame.empty()) {
    if (error) *error = "IPC frame exceeds the 4 MiB limit";
    return false;
  }
  impl_->write_queue.push_back(std::move(frame));
  impl_->flush();
  return true;
}

void IpcChannel::close() {
  impl_->close_listener();
  impl_->close_stream();
}

void IpcChannel::set_on_message(MessageHandler handler) {
  impl_->on_message = std::move(handler);
}

void IpcChannel::set_on_close(CloseHandler handler) {
  impl_->on_close = std::move(handler);
}

void IpcChannel::set_on_ready(ReadyHandler handler) {
  impl_->on_ready = std::move(handler);
}

bool IpcChannel::connected() const { return impl_->connected; }

std::string ipc_endpoint_path(const std::string& name) {
#if defined(_WIN32)
  return "\\\\.\\pipe\\uvscope-" + name;
#else
  return "/tmp/uvscope-ipc-" + name + ".sock";
#endif
}

int run_ipc_child(const std::string& endpoint) {
  uv_loop_t loop;
  if (uv_loop_init(&loop) != 0) return 1;
  IpcChannel channel(&loop);
  bool connected = false;
  channel.set_on_ready([&connected] { connected = true; });
  channel.set_on_message([&channel](const nlohmann::json& message) {
    if (!message.is_object()) return;
    const std::string type = message.value("type", std::string());
    if (type == "ping") {
      std::string error;
      channel.send({{"type", "pong"}, {"n", message.value("n", 0)}}, &error);
    } else if (type == "stop") {
      channel.close();
    }
  });
  channel.set_on_close([&loop](const std::string&) { uv_stop(&loop); });

  std::string error;
  if (!channel.connect(endpoint, &error)) {
    uv_loop_close(&loop);
    return 1;
  }
  uv_timer_t watchdog;
  uv_timer_init(&loop, &watchdog);
  watchdog.data = &loop;
  uv_timer_start(&watchdog, [](uv_timer_t* timer) { uv_stop(timer->loop); }, 15000, 0);
  uv_run(&loop, UV_RUN_DEFAULT);
  uv_timer_stop(&watchdog);
  uv_close(reinterpret_cast<uv_handle_t*>(&watchdog), nullptr);
  channel.close();
  for (int i = 0; i < 8 && uv_loop_alive(&loop); ++i) uv_run(&loop, UV_RUN_NOWAIT);
  uv_loop_close(&loop);
  return connected ? 0 : 1;
}

bool run_ipc_self_check(const std::string& executable, std::string* error) {
  uv_loop_t loop;
  if (uv_loop_init(&loop) != 0) {
    if (error) *error = "could not initialize the IPC self-check loop";
    return false;
  }
  const std::string endpoint = ipc_endpoint_path(random_hex(8));
  std::error_code ec;
  std::filesystem::remove(endpoint, ec);

  struct State {
    bool pong = false;
    bool exited = false;
    bool failed = false;
    bool process_closed = false;
    std::string failure;
    uv_process_t* child = nullptr;
  } state;

  IpcChannel server(&loop);
  // uv_stop needs the loop; capture it explicitly.
  uv_loop_t* loop_ptr = &loop;
  server.set_on_ready([&server, &state, loop_ptr] {
    std::string send_error;
    if (!server.send({{"type", "ping"}, {"n", 1}}, &send_error)) {
      state.failed = true;
      state.failure = send_error;
      uv_stop(loop_ptr);
    }
  });
  server.set_on_message([&server, &state](const nlohmann::json& message) {
    if (!message.is_object()) return;
    if (message.value("type", std::string()) == "pong" &&
        message.value("n", 0) == 1) {
      state.pong = true;
      std::string send_error;
      server.send({{"type", "stop"}}, &send_error);
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
  const char* args[] = {executable_copy.c_str(), "--ipc-child", endpoint.c_str(), nullptr};
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
    if (error) *error = std::string("could not spawn the IPC child: ") +
                        uv_strerror(spawn_rc);
    server.close();
    for (int i = 0; i < 8 && uv_loop_alive(&loop); ++i) uv_run(&loop, UV_RUN_NOWAIT);
    std::filesystem::remove(endpoint, ec);
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
    s->failure = "IPC self-check timed out";
    if (s->child != nullptr) uv_process_kill(s->child, SIGTERM);
    uv_stop(t->loop);
  }, 5000, 0);

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

  if (!state.pong) {
    if (error) *error = state.failure.empty() ? "no pong from the IPC child" : state.failure;
    return false;
  }
  if (!state.exited) {
    if (error) *error = "the IPC child did not exit";
    return false;
  }
  if (state.failed) {
    if (error) *error = state.failure;
    return false;
  }
  return true;
}

}  // namespace uvscope
