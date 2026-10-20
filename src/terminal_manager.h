#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace uvscope {

struct TerminalConfig {
  std::string name;
  std::string shell;
  std::string cwd;
  int cols = 100;
  int rows = 30;
};

// Server-side PTY sessions. Each session owns a real shell process on a
// dedicated libuv loop thread; browser disconnects do not stop it. Sessions
// live for the lifetime of the daemon (not across restarts).
class TerminalManager {
 public:
  TerminalManager();
  ~TerminalManager();

  TerminalManager(const TerminalManager&) = delete;
  TerminalManager& operator=(const TerminalManager&) = delete;

  bool available() const;
  const std::string& start_error() const;

  nlohmann::json create(std::int64_t owner_id, const std::string& owner_name,
                        const TerminalConfig& config, std::string* error);
  nlohmann::json sessions(std::int64_t owner_id, bool all) const;
  nlohmann::json session(const std::string& id) const;
  bool owner_of(const std::string& id, std::int64_t* owner_id) const;
  bool close(const std::string& id, std::string* error);
  bool input(const std::string& id, const std::string& bytes, std::string* error);
  bool resize(const std::string& id, int cols, int rows, std::string* error);
  // Ask the shell process to redraw (SIGWINCH), used when a client reattaches
  // so full-screen programs repaint without changing the PTY size.
  bool redraw(const std::string& id, std::string* error);
  nlohmann::json output(const std::string& id, std::uint64_t after, int limit) const;
  void close_all();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace uvscope
