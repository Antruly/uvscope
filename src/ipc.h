#pragma once

#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

struct uv_loop_s;

namespace uvscope {

// One length-prefixed JSON message channel over a libuv pipe. POSIX uses a
// Unix domain socket; Windows uses a named pipe. Frames are a 4-byte
// big-endian length followed by UTF-8 JSON, capped at 4 MiB.
class IpcChannel {
 public:
  using MessageHandler = std::function<void(const nlohmann::json&)>;
  using CloseHandler = std::function<void(const std::string& reason)>;
  using ReadyHandler = std::function<void()>;

  explicit IpcChannel(uv_loop_s* loop);
  ~IpcChannel();

  IpcChannel(const IpcChannel&) = delete;
  IpcChannel& operator=(const IpcChannel&) = delete;

  bool listen(const std::string& path, std::string* error);
  bool connect(const std::string& path, std::string* error);
  bool send(const nlohmann::json& message, std::string* error);
  void close();

  void set_on_message(MessageHandler handler);
  void set_on_close(CloseHandler handler);
  void set_on_ready(ReadyHandler handler);

  bool connected() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Platform pipe/socket path for a short unique name.
std::string ipc_endpoint_path(const std::string& name);

// Hidden child mode: connect to `endpoint` and answer ping/stop messages.
int run_ipc_child(const std::string& endpoint);

// Spawn `executable --ipc-child <endpoint>`, exchange ping/pong and stop it.
// Used by --self-check so the framing and lifecycle are exercised on every
// platform.
bool run_ipc_self_check(const std::string& executable, std::string* error);

}  // namespace uvscope
