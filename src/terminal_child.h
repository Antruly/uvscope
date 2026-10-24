#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "terminal_manager.h"

namespace uvscope {

int run_terminal_child(const std::string& endpoint);

// Parent-side handle for one PTY session running in a child process.
class TerminalChildService {
 public:
  TerminalChildService(std::string executable, TerminalConfig config);
  ~TerminalChildService();

  TerminalChildService(const TerminalChildService&) = delete;
  TerminalChildService& operator=(const TerminalChildService&) = delete;

  bool start(std::string* error);
  void close();
  void stop();

  bool running() const;
  bool input(const std::string& bytes, std::string* error);
  bool resize(int cols, int rows, std::string* error);
  bool redraw(std::string* error);
  nlohmann::json snapshot() const;
  nlohmann::json output(std::uint64_t after, int limit) const;
  int pid() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

nlohmann::json terminal_config_to_json(const TerminalConfig& config);
bool terminal_config_from_json(const nlohmann::json& value, TerminalConfig* config,
                               std::string* error);

}  // namespace uvscope
