#pragma once

#include <vector>

namespace uvscope {

struct TerminalCommandSeed {
  const char* command;
  const char* name_en;
  const char* name_zh;
  const char* description_en;
  const char* description_zh;
  const char* category;
  const char* platform;  // linux | windows
  const char* tags;
  bool quick;
};

const std::vector<TerminalCommandSeed>& terminal_command_seeds();

}  // namespace uvscope
