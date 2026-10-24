#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#elif !defined(_WIN32) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700
#endif

#include "terminal_manager.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
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
#include <handle/uvcpp_pipe.h>
#include <handle/uvcpp_poll.h>
#include <handle/uvcpp_timer.h>
#include <uvcpp/uvcpp_buf.h>

#include "terminal_child.h"
#include "util.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace uvscope {
namespace {

constexpr std::size_t kMaxSessions = 32;
constexpr std::size_t kMaxSessionsPerUser = 4;
constexpr std::size_t kMaxRetainedSessions = 128;
constexpr std::size_t kMaxScrollback = 256 * 1024;
constexpr std::size_t kMaxInput = 64 * 1024;
constexpr int kSweepIntervalMs = 200;

#if defined(_WIN32)
// ConPTY is available on Windows 10 1809+, but MinGW headers do not always
// declare it. Resolve the three entry points dynamically.
constexpr DWORD kProcThreadAttributePseudoConsole = 0x00020016;

struct ConPtyApi {
  HRESULT(WINAPI* create)(COORD, HANDLE, HANDLE, DWORD, void**);
  HRESULT(WINAPI* resize)(void*, COORD);
  void(WINAPI* close)(void*);
  bool ok = false;
};

ConPtyApi& conpty_api() {
  static ConPtyApi api = [] {
    ConPtyApi result{};
    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (kernel == nullptr) return result;
    result.create = reinterpret_cast<decltype(result.create)>(
        GetProcAddress(kernel, "CreatePseudoConsole"));
    result.resize = reinterpret_cast<decltype(result.resize)>(
        GetProcAddress(kernel, "ResizePseudoConsole"));
    result.close = reinterpret_cast<decltype(result.close)>(
        GetProcAddress(kernel, "ClosePseudoConsole"));
    result.ok = result.create != nullptr && result.resize != nullptr &&
                result.close != nullptr;
    return result;
  }();
  return api;
}

std::wstring widen(const std::string& value) {
  if (value.empty()) return {};
  const int needed = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
  if (needed <= 0) return {};
  std::wstring out(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      out.data(), needed);
  return out;
}
#endif

std::string default_shell() {
#if defined(_WIN32)
  // The command library is PowerShell-oriented; cmd.exe users can enter an
  // absolute shell path in the new-terminal dialog. Resolve the standard
  // PowerShell path first because MSYS2 PATHs do not always include it.
  const std::string root = env_string("SystemRoot", "C:\\Windows");
  const std::string powershell =
      root + "\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
  const DWORD attrs = GetFileAttributesW(widen(powershell).c_str());
  if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
    return powershell;
  }
  return "powershell.exe";
#else
  const std::string env = env_string("SHELL");
  if (!env.empty() && env.front() == '/' && access(env.c_str(), X_OK) == 0) return env;
  const char* candidates[] = {"/bin/bash", "/bin/zsh", "/bin/sh"};
  for (const char* path : candidates) {
    if (access(path, X_OK) == 0) return path;
  }
  return "/bin/sh";
#endif
}

bool valid_shell(const std::string& shell) {
#if defined(_WIN32)
  if (shell.empty()) return false;
  const bool has_separator = shell.find('\\') != std::string::npos ||
                             shell.find('/') != std::string::npos ||
                             shell.find(':') != std::string::npos;
  if (!has_separator) return true;  // resolved through PATH by CreateProcess.
  const DWORD attrs = GetFileAttributesW(widen(shell).c_str());
  return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
  if (shell.empty() || shell.front() != '/') return false;
  return access(shell.c_str(), X_OK) == 0;
#endif
}

bool valid_cwd(const std::string& cwd) {
#if defined(_WIN32)
  if (cwd.empty()) return false;
  const DWORD attrs = GetFileAttributesW(widen(cwd).c_str());
  return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
  if (cwd.empty() || cwd.front() != '/') return false;
  struct stat st;
  return stat(cwd.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

std::string default_cwd() {
#if defined(_WIN32)
  const std::string profile = env_string("USERPROFILE");
  if (valid_cwd(profile)) return profile;
  return "C:\\";
#else
  const std::string home = env_string("HOME");
  if (valid_cwd(home)) return home;
  return "/";
#endif
}

int clamp_int(int value, int low, int high) {
  return std::max(low, std::min(value, high));
}

}  // namespace

struct TerminalManager::Impl {
  struct Chunk {
    std::uint64_t seq = 0;
    std::string data;
  };

  struct Session {
    std::string id;
    // The PTY lives in a child process. The handle can be null between a
    // failed restart and the supervisor's next attempt, so the flag is
    // authoritative.
    bool isolated = false;
    std::shared_ptr<TerminalChildService> child;
    std::int64_t owner_id = 0;
    std::string owner_name;
    std::string name;
    std::string shell;
    std::string cwd;
    std::string state = "starting";  // starting | running | closing | exited | error
    // Set by DELETE: hide the session at once, reap the child, then drop it
    // from the registry. Exited sessions that were not deleted stay visible so
    // their scrollback can still be inspected.
    bool remove_on_close = false;
    std::string error;
    std::string created_at;
    std::string exited_at;
    std::string last_activity;
    int pid = -1;
    int fd = -1;
    int exit_code = -1;
    int cols = 100;
    int rows = 30;
    std::uint64_t next_seq = 1;
    std::deque<Chunk> chunks;
    std::size_t buffered_bytes = 0;
    uvcpp::uvcpp_poll* poll = nullptr;
    bool polling = false;
    std::string pending_input;
    std::mutex* state_mutex = nullptr;
#if defined(_WIN32)
    void* hpc = nullptr;
    void* process = nullptr;
    void* input_write = nullptr;
    uvcpp::uvcpp_pipe* out_pipe = nullptr;
    void* drain_handle = nullptr;
    std::int64_t hpc_close_at_ms = 0;
    std::atomic<std::uint64_t> input_written{0};
    std::atomic<std::uint64_t> output_read{0};
    std::atomic<int> last_input_error{0};
#endif
  };

  static nlohmann::json snapshot_locked(const Session& session);

  struct CreateState {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    std::string error;
  };

  std::string executable_path;

  std::thread thread;
  std::mutex ready_mutex;
  std::condition_variable ready_cv;
  bool ready = false;
  std::string start_error;
  bool available = false;

  uvcpp::uvcpp_loop* loop = nullptr;
  uvcpp::uvcpp_async* wake = nullptr;
  uvcpp::uvcpp_timer* sweep_timer = nullptr;

  std::mutex command_mutex;
  bool accepting = true;
  std::vector<std::function<void()>> commands;

  mutable std::mutex state_mutex;
  std::map<std::string, std::shared_ptr<Session>> sessions;
  std::mutex retired_mutex;
  std::vector<std::thread> retired;

  bool post(std::function<void()> fn) {
    {
      std::lock_guard<std::mutex> lock(command_mutex);
      if (!accepting) return false;
      commands.push_back(std::move(fn));
    }
    if (wake != nullptr) wake->send();
    return true;
  }

  void drain() {
    std::vector<std::function<void()>> pending;
    {
      std::lock_guard<std::mutex> lock(command_mutex);
      pending.swap(commands);
    }
    for (auto& fn : pending) fn();
  }

  std::shared_ptr<Session> session_locked(const std::string& id) const {
    const auto it = sessions.find(id);
    if (it == sessions.end()) return nullptr;
    // A deleted terminal is already gone from the API's point of view; the
    // sweep timer keeps its shared_ptr alive only long enough to reap the
    // child and close the PTY.
    if (it->second->state == "closing") return nullptr;
    return it->second;
  }

  static void append_output_unlocked(Session& session, const char* data,
                                     std::size_t len) {
    if (len == 0) return;
    session.chunks.push_back({session.next_seq++, std::string(data, len)});
    session.buffered_bytes += len;
    while (session.buffered_bytes > kMaxScrollback) {
      if (session.chunks.empty()) break;
      session.buffered_bytes -= session.chunks.front().data.size();
      session.chunks.pop_front();
    }
    session.last_activity = now_iso8601();
  }

  static void append_output(Session& session, const char* data, std::size_t len) {
    std::unique_lock<std::mutex> lock;
    if (session.state_mutex != nullptr) lock = std::unique_lock<std::mutex>(*session.state_mutex);
    append_output_unlocked(session, data, len);
  }

  static void close_fd(int& fd) {
#if !defined(_WIN32)
    if (fd >= 0) ::close(fd);
#endif
    fd = -1;
  }

  static void close_poll(Session& session) {
    if (session.poll == nullptr) return;
    uvcpp::uvcpp_poll* handle = session.poll;
    session.poll = nullptr;
    session.polling = false;
    handle->set_data(nullptr);
    handle->stop();
    handle->close([](uvcpp::uvcpp_handle* raw) {
      delete static_cast<uvcpp::uvcpp_poll*>(raw);
    });
  }


#if defined(_WIN32)
  static void close_pipe(uvcpp::uvcpp_pipe*& pipe) {
    if (pipe == nullptr) return;
    uvcpp::uvcpp_pipe* handle = pipe;
    pipe = nullptr;
    handle->set_data(nullptr);
    handle->read_stop();
    handle->close([](uvcpp::uvcpp_handle* raw) {
      delete static_cast<uvcpp::uvcpp_pipe*>(raw);
    });
  }

  static void conpty_alloc(uvcpp::uvcpp_handle*, size_t suggested, uv_buf_t* buf) {
    const size_t size = suggested > 0 ? suggested : 16384;
    buf->base = new char[size];
    buf->len = size;
  }

  static void conpty_read(uvcpp::uvcpp_stream* stream, ssize_t nread,
                          const uv_buf_t* buf) {
    auto* session = static_cast<Session*>(stream->get_data());
    if (buf != nullptr && buf->base != nullptr) {
      if (nread > 0 && session != nullptr) {
        append_output(*session, buf->base, static_cast<std::size_t>(nread));
        session->output_read.fetch_add(static_cast<std::uint64_t>(nread));
      }
      delete[] buf->base;
    }
    if (nread < 0 && session != nullptr) {
      DWORD process_status = WAIT_FAILED;
      DWORD exit_code = 0;
      if (session->process != nullptr) {
        process_status = WaitForSingleObject(session->process, 0);
        if (process_status == WAIT_OBJECT_0) {
          GetExitCodeProcess(session->process, &exit_code);
        }
      }
      close_pipe(session->out_pipe);
      std::lock_guard<std::mutex> lock(*session->state_mutex);
      std::fprintf(stderr,
                   "uvscope: terminal output closed (status=%ld processWait=%lu "
                   "exitCode=%lu state=%s)\n",
                   static_cast<long>(nread),
                   static_cast<unsigned long>(process_status),
                   static_cast<unsigned long>(exit_code), session->state.c_str());
      std::fflush(stderr);
      if (process_status != WAIT_OBJECT_0 && session->state == "running") {
        session->state = "error";
        session->error = "console output closed";
        session->exited_at = now_iso8601();
      }
    }
  }

  static void pump_input(Session& session) {
    if (session.pending_input.empty() || session.input_write == nullptr) return;
    std::string data;
    data.swap(session.pending_input);
    std::size_t offset = 0;
    while (offset < data.size()) {
      DWORD written = 0;
      const DWORD chunk = static_cast<DWORD>(data.size() - offset);
      if (!WriteFile(static_cast<HANDLE>(session.input_write), data.data() + offset,
                     chunk, &written, nullptr) ||
          written == 0) {
        break;
      }
      offset += written;
    }
    if (offset < data.size()) {
      session.pending_input = data.substr(offset) + session.pending_input;
    }
    session.input_written.fetch_add(offset);
    if (offset < data.size()) session.last_input_error.store(static_cast<int>(GetLastError()));
  }

  static std::size_t drain_remaining_output(Session& session) {
    if (session.drain_handle == nullptr) return 0;
    std::size_t total = 0;
    char buffer[16384];
    for (;;) {
      DWORD available = 0;
      if (!PeekNamedPipe(static_cast<HANDLE>(session.drain_handle), nullptr, 0,
                         nullptr, &available, nullptr) ||
          available == 0) {
        break;
      }
      const DWORD want =
          available < sizeof(buffer) ? available : static_cast<DWORD>(sizeof(buffer));
      DWORD read = 0;
      if (!ReadFile(static_cast<HANDLE>(session.drain_handle), buffer, want, &read,
                    nullptr) ||
          read == 0) {
        break;
      }
      append_output_unlocked(session, buffer, read);
      session.output_read.fetch_add(read);
      total += read;
    }
    CloseHandle(static_cast<HANDLE>(session.drain_handle));
    session.drain_handle = nullptr;
    return total;
  }

  static void close_windows_session(Session& session) {
    if (session.process != nullptr) {
      TerminateProcess(session.process, 1);
      CloseHandle(session.process);
      session.process = nullptr;
    }
    if (session.input_write != nullptr) {
      CloseHandle(static_cast<HANDLE>(session.input_write));
      session.input_write = nullptr;
    }
    if (session.drain_handle != nullptr) {
      CloseHandle(static_cast<HANDLE>(session.drain_handle));
      session.drain_handle = nullptr;
    }
    close_pipe(session.out_pipe);
    if (session.hpc != nullptr) {
      if (conpty_api().close != nullptr) conpty_api().close(session.hpc);
      session.hpc = nullptr;
    }
  }

  static void spawn_conpty(const std::shared_ptr<Session>& session,
                           const std::shared_ptr<CreateState>& state) {
    auto fail = [&state](const std::string& message) {
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->error = message;
        state->done = true;
      }
      state->cv.notify_all();
    };
    if (!conpty_api().ok || current_loop == nullptr) {
      fail("ConPTY is not available on this Windows version");
      return;
    }

    HANDLE input_read = nullptr;
    HANDLE input_write = nullptr;
    HANDLE output_read = nullptr;
    HANDLE output_write = nullptr;
    if (!CreatePipe(&input_read, &input_write, nullptr, 0) ||
        !CreatePipe(&output_read, &output_write, nullptr, 0)) {
      if (input_read) CloseHandle(input_read);
      if (input_write) CloseHandle(input_write);
      if (output_read) CloseHandle(output_read);
      if (output_write) CloseHandle(output_write);
      fail("CreatePipe failed: " + std::to_string(GetLastError()));
      return;
    }

    void* hpc = nullptr;
    COORD size;
    size.X = static_cast<SHORT>(session->cols);
    size.Y = static_cast<SHORT>(session->rows);
    const HRESULT created =
        conpty_api().create(size, input_read, output_write, 0, &hpc);
    CloseHandle(input_read);
    CloseHandle(output_write);
    if (FAILED(created) || hpc == nullptr) {
      CloseHandle(input_write);
      CloseHandle(output_read);
      fail("CreatePseudoConsole failed: " + std::to_string(created));
      return;
    }

    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    auto* attr = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        HeapAlloc(GetProcessHeap(), 0, attr_size));
    const bool attr_initialized =
        attr != nullptr &&
        InitializeProcThreadAttributeList(attr, 1, 0, &attr_size) != 0;
    const bool attr_ok =
        attr_initialized &&
        UpdateProcThreadAttribute(attr, 0, kProcThreadAttributePseudoConsole, hpc,
                                  sizeof(hpc), nullptr, nullptr) != 0;
    if (!attr_ok) {
      if (attr_initialized) DeleteProcThreadAttributeList(attr);
      if (attr) HeapFree(GetProcessHeap(), 0, attr);
      CloseHandle(input_write);
      CloseHandle(output_read);
      conpty_api().close(hpc);
      fail("cannot attach the ConPTY attribute list");
      return;
    }

    STARTUPINFOEXW startup;
    std::memset(&startup, 0, sizeof(startup));
    startup.StartupInfo.cb = sizeof(startup);
    // Without STARTF_USESTDHANDLES, CreateProcess hands the child the parent's
    // redirected std handles. The daemon's stdout/stderr point at a log file
    // whenever it is started with redirection, so PowerShell would write its
    // banner there, never touch the pseudoconsole, and exit on stdin EOF.
    // Selecting the fields while leaving them null forces the child to use the
    // console created by PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE instead
    // (microsoft/terminal#11276).
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nullptr;
    startup.StartupInfo.hStdOutput = nullptr;
    startup.StartupInfo.hStdError = nullptr;
    startup.lpAttributeList = attr;
    PROCESS_INFORMATION process_info;
    std::memset(&process_info, 0, sizeof(process_info));
    const std::wstring shell = widen(session->shell);
    const std::wstring cwd = widen(session->cwd);
    const bool has_path = session->shell.find('\\') != std::string::npos ||
                          session->shell.find('/') != std::string::npos ||
                          session->shell.find(':') != std::string::npos;
    std::wstring command_line = L"\"" + shell + L"\"";
    std::vector<wchar_t> command(command_line.begin(), command_line.end());
    command.push_back(L'\0');
    const BOOL launched = CreateProcessW(
        has_path ? shell.c_str() : nullptr, command.data(), nullptr, nullptr, FALSE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, nullptr,
        cwd.empty() ? nullptr : cwd.c_str(), &startup.StartupInfo, &process_info);
    DeleteProcThreadAttributeList(attr);
    HeapFree(GetProcessHeap(), 0, attr);
    if (!launched) {
      CloseHandle(input_write);
      CloseHandle(output_read);
      conpty_api().close(hpc);
      fail("CreateProcess failed: " + std::to_string(GetLastError()));
      return;
    }
    CloseHandle(process_info.hThread);

    HANDLE drain_handle = nullptr;
    DuplicateHandle(GetCurrentProcess(), output_read, GetCurrentProcess(),
                    &drain_handle, 0, FALSE, DUPLICATE_SAME_ACCESS);
    const int out_fd = _open_osfhandle(
        reinterpret_cast<intptr_t>(output_read), _O_RDONLY | _O_BINARY);
    if (out_fd < 0) {
      CloseHandle(output_read);
      CloseHandle(input_write);
      if (drain_handle) CloseHandle(drain_handle);
      TerminateProcess(process_info.hProcess, 1);
      CloseHandle(process_info.hProcess);
      conpty_api().close(hpc);
      fail("cannot adapt the ConPTY output pipe");
      return;
    }

    auto* out_pipe = new uvcpp::uvcpp_pipe(current_loop, 0);
    out_pipe->set_data(session.get());
    if (out_pipe->open(out_fd) != 0) {
      out_pipe->close([](uvcpp::uvcpp_handle* raw) {
        delete static_cast<uvcpp::uvcpp_pipe*>(raw);
      });
      _close(out_fd);
      if (drain_handle) CloseHandle(drain_handle);
      CloseHandle(input_write);
      TerminateProcess(process_info.hProcess, 1);
      CloseHandle(process_info.hProcess);
      conpty_api().close(hpc);
      fail("cannot open the ConPTY output pipe");
      return;
    }
    out_pipe->read_start(conpty_alloc, conpty_read);

    session->hpc = hpc;
    session->process = process_info.hProcess;
    session->input_write = input_write;
    session->out_pipe = out_pipe;
    session->drain_handle = drain_handle;
    session->state = "running";
    session->created_at = now_iso8601();
    session->last_activity = session->created_at;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->done = true;
    }
    state->cv.notify_all();
  }
#endif

  static void read_output(Session& session) {
#if defined(_WIN32)
    (void)session;
#else
    char buffer[16384];
    for (;;) {
      const ssize_t n = read(session.fd, buffer, sizeof(buffer));
      if (n > 0) {
        append_output(session, buffer, static_cast<std::size_t>(n));
        continue;
      }
      if (n == 0) break;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      if (errno == EINTR) continue;
      // EIO is the normal PTY close signal after the child exits.
      break;
    }
    close_poll(session);
#endif
  }

  static void flush_input(Session& session) {
#if defined(_WIN32)
    (void)session;
#else
    while (!session.pending_input.empty()) {
      const ssize_t n = write(session.fd, session.pending_input.data(),
                              session.pending_input.size());
      if (n > 0) {
        session.pending_input.erase(0, static_cast<std::size_t>(n));
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
      if (n < 0 && errno == EINTR) continue;
      session.pending_input.clear();
      break;
    }
    if (session.poll == nullptr) return;
    if (session.pending_input.empty()) {
      session.poll->start(UV_READABLE, on_poll);
    }
#endif
  }

  static void on_poll(uvcpp::uvcpp_poll* handle, int status, int events) {
    auto* session = static_cast<Session*>(handle->get_data());
    if (session == nullptr) return;
    if (status < 0) {
      close_poll(*session);
      return;
    }
    if (events & UV_READABLE) read_output(*session);
    if (session->poll != nullptr && (events & UV_WRITABLE)) flush_input(*session);
  }

  static void start_poll(Session& session) {
    if (session.fd < 0 || session.poll != nullptr || current_loop == nullptr) return;
    auto* handle = new uvcpp::uvcpp_poll(
        current_loop, static_cast<uv_os_sock_t>(session.fd));
    handle->set_data(&session);
    session.poll = handle;
    session.polling = true;
    if (handle->start(UV_READABLE, on_poll) != 0) close_poll(session);
  }

  static thread_local uvcpp::uvcpp_loop* current_loop;

  static void reap(Session& session) {
#if defined(_WIN32)
    if (session.process == nullptr) return;
    if (WaitForSingleObject(session.process, 0) != WAIT_OBJECT_0) return;
    DWORD code = 0;
    GetExitCodeProcess(session.process, &code);
    if (session.state != "exited") {
      session.state = "exited";
      session.exited_at = now_iso8601();
      session.exit_code = static_cast<int>(code);
    }
    CloseHandle(session.process);
    session.process = nullptr;
    if (session.input_write != nullptr) {
      CloseHandle(static_cast<HANDLE>(session.input_write));
      session.input_write = nullptr;
    }
    // Stop the libuv reader, then drain whatever is still in the pipe
    // through the duplicated handle before closing the pseudoconsole.
    close_pipe(session.out_pipe);
    const std::size_t drained = drain_remaining_output(session);
    std::fprintf(stderr, "uvscope: terminal exit code=%lu drained=%zu bytes\n",
                 static_cast<unsigned long>(code), drained);
    std::fflush(stderr);
    if (session.hpc != nullptr) {
      if (conpty_api().close != nullptr) conpty_api().close(session.hpc);
      session.hpc = nullptr;
    }
    session.hpc_close_at_ms = 0;
#else
    if (session.pid <= 0) return;
    int status = 0;
    const pid_t pid = waitpid(session.pid, &status, WNOHANG);
    if (pid <= 0) return;
    if (session.state != "exited") {
      session.state = "exited";
      session.exited_at = now_iso8601();
      if (WIFEXITED(status)) {
        session.exit_code = WEXITSTATUS(status);
      } else if (WIFSIGNALED(status)) {
        session.exit_code = 128 + WTERMSIG(status);
      }
    }
    session.pid = -1;
    close_poll(session);
    close_fd(session.fd);
#endif
  }

  void sweep() {
    std::vector<std::shared_ptr<Session>> retiring;
    {
      std::lock_guard<std::mutex> lock(state_mutex);
      for (auto it = sessions.begin(); it != sessions.end();) {
        Session& session = *it->second;
        if (session.child) {
          // The child owns the PTY state; mirror it so retention pruning and
          // the API see exits without a second state machine in the parent.
          const auto snapshot = session.child->snapshot();
          session.state = snapshot.value("state", session.state);
          session.error = snapshot.value("error", session.error);
          session.cols = snapshot.value("cols", session.cols);
          session.rows = snapshot.value("rows", session.rows);
        } else if (session.state == "running" || session.state == "closing") {
          reap(session);
        }
#if defined(_WIN32)
        if (session.hpc != nullptr &&
            (session.state == "exited" || session.state == "error") &&
            session.hpc_close_at_ms > 0) {
          const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
          if (now >= session.hpc_close_at_ms) {
            if (conpty_api().close != nullptr) conpty_api().close(session.hpc);
            session.hpc = nullptr;
          }
        }
#endif
        // DELETE: once the child is reaped and the PTY handles are closed the
        // session has no resources left, so drop it from the registry.
        const bool reaped =
#if defined(_WIN32)
            session.process == nullptr && session.out_pipe == nullptr;
#else
            session.pid <= 0;
#endif
        if (session.remove_on_close && reaped) {
          if (session.child) retiring.push_back(it->second);
          it = sessions.erase(it);
          continue;
        }
        ++it;
      }
      // Keep a short history of exited sessions without unbounded growth.
      while (sessions.size() > kMaxRetainedSessions) {
        auto victim = sessions.end();
        for (auto it = sessions.begin(); it != sessions.end(); ++it) {
          if (it->second->state == "exited" || it->second->state == "error") {
            victim = it;
            break;
          }
        }
        if (victim == sessions.end()) break;
        if (victim->second->child) retiring.push_back(victim->second);
        sessions.erase(victim);
      }
    }
    for (const auto& session : retiring) retire(session);
  }

  // Stopping an isolated session joins its IPC thread and reaps the process;
  // do that off the manager loop so pruning cannot stall other terminals.
  void retire(const std::shared_ptr<Session>& session) {
    auto child = session->child;
    if (!child) return;
    std::lock_guard<std::mutex> lock(retired_mutex);
    retired.emplace_back([child] { child->stop(); });
  }

  void join_retired() {
    std::vector<std::thread> threads;
    {
      std::lock_guard<std::mutex> lock(retired_mutex);
      threads.swap(retired);
    }
    for (auto& thread : threads) {
      if (thread.joinable()) thread.join();
    }
  }

  static void fork_session(const std::shared_ptr<Session>& session,
                           const std::shared_ptr<CreateState>& state) {
#if defined(_WIN32)
    spawn_conpty(session, state);
    return;
#else
    struct winsize ws;
    std::memset(&ws, 0, sizeof(ws));
    ws.ws_col = static_cast<unsigned short>(session->cols);
    ws.ws_row = static_cast<unsigned short>(session->rows);

    // Build argv/envp before fork; the child only calls async-signal-safe
    // functions (chdir/execve) before _exit.
    const char* shell = session->shell.c_str();
    char* argv[] = {const_cast<char*>(shell), nullptr};
    std::vector<std::string> env_storage;
    auto add_env = [&env_storage](const std::string& key, const std::string& value) {
      if (!key.empty() && !value.empty()) env_storage.push_back(key + "=" + value);
    };
    add_env("TERM", "xterm-256color");
    add_env("COLORTERM", "truecolor");
    add_env("SHELL", session->shell);
    add_env("HOME", env_string("HOME"));
    add_env("USER", env_string("USER"));
    add_env("LOGNAME", env_string("LOGNAME"));
    add_env("PATH", env_string("PATH", "/usr/local/bin:/usr/bin:/bin"));
    add_env("LANG", env_string("LANG"));
    add_env("LC_ALL", env_string("LC_ALL"));
    add_env("UVSCOPE_TERMINAL", "1");
    std::vector<char*> envp;
    envp.reserve(env_storage.size() + 1);
    for (auto& item : env_storage) envp.push_back(item.data());
    envp.push_back(nullptr);

    // Use posix_openpt/fork instead of forkpty: macOS does not ship a
    // standalone libutil that CMake can reliably find, while posix_openpt is
    // in libc on both Linux and macOS. The slave name is resolved before the
    // fork so the child only performs async-signal-safe calls.
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
      if (master >= 0) ::close(master);
      std::lock_guard<std::mutex> lock(state->mutex);
      state->error = "posix_openpt failed: " + std::string(std::strerror(errno));
      state->done = true;
      state->cv.notify_all();
      return;
    }
    const char* resolved_slave = ptsname(master);
    if (resolved_slave == nullptr) {
      ::close(master);
      std::lock_guard<std::mutex> lock(state->mutex);
      state->error = "ptsname failed: " + std::string(std::strerror(errno));
      state->done = true;
      state->cv.notify_all();
      return;
    }
    char slave_name[256] = {};
    std::strncpy(slave_name, resolved_slave, sizeof(slave_name) - 1);

    const pid_t pid = fork();
    if (pid < 0) {
      ::close(master);
      std::lock_guard<std::mutex> lock(state->mutex);
      state->error = "fork failed: " + std::string(std::strerror(errno));
      state->done = true;
      state->cv.notify_all();
      return;
    }
    if (pid == 0) {
      setsid();
      const int slave = open(slave_name, O_RDWR);
      if (slave < 0) _exit(126);
      ioctl(slave, TIOCSCTTY, 0);
      ioctl(slave, TIOCSWINSZ, &ws);
      dup2(slave, STDIN_FILENO);
      dup2(slave, STDOUT_FILENO);
      dup2(slave, STDERR_FILENO);
      if (slave > STDERR_FILENO) ::close(slave);
      if (chdir(session->cwd.c_str()) != 0) _exit(126);
      execve(session->shell.c_str(), argv, envp.data());
      _exit(127);
    }

    const int flags = fcntl(master, F_GETFL, 0);
    if (flags >= 0) fcntl(master, F_SETFL, flags | O_NONBLOCK);

    session->pid = pid;
    session->fd = master;
    session->state = "running";
    session->created_at = now_iso8601();
    session->last_activity = session->created_at;
    start_poll(*session);

    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->done = true;
    }
    state->cv.notify_all();
#endif
  }

  void run_loop() {
    auto* created_loop = new uvcpp::uvcpp_loop();
    loop = created_loop;
    current_loop = loop;

    auto* created_wake = new uvcpp::uvcpp_async();
    if (created_wake->init([this](uvcpp::uvcpp_async*) { drain(); }, loop) != 0) {
      delete created_wake;
      loop->loop_close();
      delete loop;
      loop = nullptr;
      current_loop = nullptr;
      {
        std::lock_guard<std::mutex> lock(ready_mutex);
        start_error = "cannot initialize the terminal wake handle";
        ready = true;
      }
      ready_cv.notify_all();
      return;
    }
    wake = created_wake;

    auto* timer = new uvcpp::uvcpp_timer(loop);
    timer->set_data(this);
    if (timer->start([](uvcpp::uvcpp_timer* handle) {
          static_cast<Impl*>(handle->get_data())->sweep();
        }, kSweepIntervalMs, kSweepIntervalMs) != 0) {
      timer->close([](uvcpp::uvcpp_handle* raw) {
        delete static_cast<uvcpp::uvcpp_timer*>(raw);
      });
      delete wake;
      wake = nullptr;
      // Let the deferred close callbacks run before the loop is torn down.
      loop->run(UV_RUN_NOWAIT);
      loop->loop_close();
      delete loop;
      loop = nullptr;
      current_loop = nullptr;
      std::lock_guard<std::mutex> lock(ready_mutex);
      start_error = "cannot initialize the terminal timer";
      ready = true;
      ready_cv.notify_all();
      return;
    }
    sweep_timer = timer;

    available = true;
    {
      std::lock_guard<std::mutex> lock(ready_mutex);
      ready = true;
    }
    ready_cv.notify_all();

    loop->run(UV_RUN_DEFAULT);

    {
      std::lock_guard<std::mutex> lock(state_mutex);
      for (auto& item : sessions) {
        Session& session = *item.second;
        close_poll(session);
        close_fd(session.fd);
#if defined(_WIN32)
        close_windows_session(session);
#else
        if (session.pid > 0) {
          kill(session.pid, SIGHUP);
          int status = 0;
          waitpid(session.pid, &status, WNOHANG);
          session.pid = -1;
        }
#endif
      }
    }
    if (sweep_timer != nullptr) {
      sweep_timer->close([](uvcpp::uvcpp_handle* raw) {
        delete static_cast<uvcpp::uvcpp_timer*>(raw);
      });
      sweep_timer = nullptr;
    }
    if (wake != nullptr) {
      delete wake;
      wake = nullptr;
    }
    // uv_close is asynchronous: give the loop one pass so the handles queued
    // above (timer, wake, pipes and polls) are freed before loop_close().
    loop->run(UV_RUN_NOWAIT);
    loop->loop_close();
    delete loop;
    loop = nullptr;
    current_loop = nullptr;
  }
};

thread_local uvcpp::uvcpp_loop* TerminalManager::Impl::current_loop = nullptr;

TerminalManager::TerminalManager(std::string executable_path) : impl_(new Impl()) {
  impl_->executable_path = std::move(executable_path);
#if defined(_WIN32)
  if (!conpty_api().ok) {
    impl_->start_error = "ConPTY is not available on this Windows version";
    return;
  }
#endif
  impl_->thread = std::thread([this] { impl_->run_loop(); });
  std::unique_lock<std::mutex> lock(impl_->ready_mutex);
  impl_->ready_cv.wait(lock, [this] { return impl_->ready; });
  if (!impl_->start_error.empty()) impl_->available = false;
}

TerminalManager::~TerminalManager() {
  std::vector<std::shared_ptr<Impl::Session>> isolated;
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    for (const auto& item : impl_->sessions) {
      if (item.second->child) isolated.push_back(item.second);
    }
  }
  for (const auto& session : isolated) session->child->stop();
  if (!impl_->thread.joinable()) return;
  {
    std::lock_guard<std::mutex> lock(impl_->command_mutex);
    impl_->accepting = false;
    impl_->commands.push_back([this] {
      {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        for (auto& item : impl_->sessions) {
          Impl::Session& session = *item.second;
          Impl::close_poll(session);
          Impl::close_fd(session.fd);
#if defined(_WIN32)
          Impl::close_windows_session(session);
#else
          if (session.pid > 0) {
            kill(session.pid, SIGHUP);
            int status = 0;
            waitpid(session.pid, &status, WNOHANG);
            session.pid = -1;
          }
#endif
        }
      }
      if (impl_->loop != nullptr) impl_->loop->stop();
    });
  }
  if (impl_->wake != nullptr) impl_->wake->send();
  impl_->thread.join();
  impl_->join_retired();
}

bool TerminalManager::available() const { return impl_ && impl_->available; }

const std::string& TerminalManager::start_error() const { return impl_->start_error; }

nlohmann::json TerminalManager::create(std::int64_t owner_id,
                                       const std::string& owner_name,
                                       const TerminalConfig& config,
                                       std::string* error) {
  if (!available()) {
    if (error) *error = start_error().empty() ? "terminal is unavailable" : start_error();
    return nullptr;
  }
  TerminalConfig options = config;
  if (options.shell.empty()) options.shell = default_shell();
  if (!valid_shell(options.shell)) {
    if (error) *error = "shell must be an executable path or command name";
    return nullptr;
  }
  if (options.cwd.empty()) options.cwd = default_cwd();
  if (!valid_cwd(options.cwd)) {
    if (error) *error = "cwd must be an absolute directory";
    return nullptr;
  }
  if (options.name.empty()) options.name = "Terminal";
  if (options.name.size() > 80) options.name = options.name.substr(0, 80);
  options.cols = clamp_int(options.cols, 20, 500);
  options.rows = clamp_int(options.rows, 5, 200);

  auto session = std::make_shared<Impl::Session>();
  session->id = make_id();
  session->owner_id = owner_id;
  session->owner_name = owner_name;
  session->name = options.name;
  session->shell = options.shell;
  session->cwd = options.cwd;
  session->cols = options.cols;
  session->rows = options.rows;
  session->isolated = options.isolated;
  session->state_mutex = &impl_->state_mutex;
  session->created_at = now_iso8601();

  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    std::size_t total = 0;
    std::size_t mine = 0;
    for (const auto& item : impl_->sessions) {
      if (item.second->state == "running" || item.second->state == "starting") {
        ++total;
        if (item.second->owner_id == owner_id) ++mine;
      }
    }
    if (total >= kMaxSessions) {
      if (error) *error = "too many terminal sessions";
      return nullptr;
    }
    if (mine >= kMaxSessionsPerUser) {
      if (error) *error = "too many terminal sessions for this user";
      return nullptr;
    }
    impl_->sessions[session->id] = session;
  }

  if (options.isolated) {
    auto child =
        std::make_shared<TerminalChildService>(impl_->executable_path, options);
    std::string child_error;
    if (!child->start(&child_error)) {
      {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        impl_->sessions.erase(session->id);
      }
      if (error) {
        *error = child_error.empty() ? "terminal child failed to start" : child_error;
      }
      return nullptr;
    }
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    session->child = std::move(child);
    const auto snapshot = session->child->snapshot();
    session->state = snapshot.value("state", std::string("running"));
    session->error = snapshot.value("error", std::string());
    if (session->state == "error") {
      impl_->sessions.erase(session->id);
      if (error) {
        *error = session->error.empty() ? "terminal child failed to start"
                                        : session->error;
      }
      return nullptr;
    }
    const auto stored = impl_->session_locked(session->id);
    return stored ? nlohmann::json{{"ok", true},
                                   {"session", Impl::snapshot_locked(*stored)}}
                  : nullptr;
  }

  auto ready = std::make_shared<Impl::CreateState>();
  if (!impl_->post([this, session, ready] {
        Impl::fork_session(session, ready);
      })) {
    {
      std::lock_guard<std::mutex> lock(impl_->state_mutex);
      impl_->sessions.erase(session->id);
    }
    if (error) *error = "terminal manager is stopping";
    return nullptr;
  }

  bool done = false;
  {
    std::unique_lock<std::mutex> lock(ready->mutex);
    done = ready->cv.wait_for(lock, std::chrono::seconds(5), [&ready] { return ready->done; });
  }
  if (!done) {
    {
      std::lock_guard<std::mutex> lock(impl_->state_mutex);
      impl_->sessions.erase(session->id);
    }
    if (error) *error = "terminal did not start in time";
    return nullptr;
  }
  if (!ready->error.empty()) {
    {
      std::lock_guard<std::mutex> lock(impl_->state_mutex);
      impl_->sessions.erase(session->id);
    }
    if (error) *error = ready->error;
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  const auto stored = impl_->session_locked(session->id);
  return stored ? nlohmann::json{{"ok", true}, {"session", Impl::snapshot_locked(*stored)}} : nullptr;
}

nlohmann::json TerminalManager::Impl::snapshot_locked(const Session& session) {
  nlohmann::json out = {{"id", session.id},
                        {"ownerId", session.owner_id},
                        {"owner", session.owner_name},
                        {"name", session.name},
                        {"shell", session.shell},
                        {"cwd", session.cwd},
                        {"state", session.state},
                        {"error", session.error},
                        {"createdAt", session.created_at},
                        {"exitedAt", session.exited_at},
                        {"lastActivity", session.last_activity},
                        {"exitCode", session.exit_code},
                        {"cols", session.cols},
                        {"rows", session.rows},
                        {"nextSeq", session.next_seq - 1},
                        {"bufferedBytes", session.buffered_bytes},
                        {"isolated", session.isolated},
#if defined(_WIN32)
                        {"inputBytesWritten", session.input_written.load()},
                        {"outputBytesRead", session.output_read.load()},
                        {"lastInputError", session.last_input_error.load()},
#endif
  };
  if (session.child) {
    // The child owns the authoritative PTY state and scrollback counters; the
    // parent keeps its own id/owner/name around them.
    const auto child = session.child->snapshot();
    for (const char* key : {"state", "error", "exitedAt", "lastActivity",
                            "exitCode", "cols", "rows", "nextSeq",
                            "bufferedBytes"}) {
      if (child.contains(key)) out[key] = child[key];
    }
    out["pid"] = child.value("pid", 0);
  }
  return out;
}

nlohmann::json TerminalManager::sessions(std::int64_t owner_id, bool all) const {
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  nlohmann::json out = nlohmann::json::array();
  for (const auto& item : impl_->sessions) {
    if (item.second->state == "closing") continue;
    if (!all && item.second->owner_id != owner_id) continue;
    out.push_back(Impl::snapshot_locked(*item.second));
  }
  std::sort(out.begin(), out.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
    return a.value("createdAt", std::string()) > b.value("createdAt", std::string());
  });
  return {{"ok", true}, {"sessions", out}};
}

nlohmann::json TerminalManager::session(const std::string& id) const {
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  const auto session = impl_->session_locked(id);
  if (!session) return {{"ok", false}, {"error", "terminal not found"}};
  return {{"ok", true}, {"session", Impl::snapshot_locked(*session)}};
}

bool TerminalManager::owner_of(const std::string& id, std::int64_t* owner_id) const {
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  const auto session = impl_->session_locked(id);
  if (!session) return false;
  if (owner_id) *owner_id = session->owner_id;
  return true;
}

bool TerminalManager::close(const std::string& id, std::string* error) {
  std::shared_ptr<Impl::Session> session;
  std::shared_ptr<TerminalChildService> child;
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    session = impl_->session_locked(id);
    if (session) child = session->child;
  }
  if (!session) {
    if (error) *error = "terminal not found";
    return false;
  }
  if (session->isolated) {
    if (child) child->close();
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    impl_->sessions.erase(id);
    return true;
  }
  if (!impl_->post([this, session] {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        if (session->state == "running" || session->state == "starting") {
          session->state = "closing";
        }
        Impl::close_poll(*session);
        Impl::close_fd(session->fd);
#if defined(_WIN32)
        if (session->process != nullptr) {
          TerminateProcess(session->process, 1);
        }
        if (session->input_write != nullptr) {
          CloseHandle(static_cast<HANDLE>(session->input_write));
          session->input_write = nullptr;
        }
        if (session->hpc != nullptr) {
          if (conpty_api().close != nullptr) conpty_api().close(session->hpc);
          session->hpc = nullptr;
        }
#else
        if (session->pid > 0) {
          kill(session->pid, SIGHUP);
        }
#endif
      })) {
    if (error) *error = "terminal manager is stopping";
    return false;
  }
  // DELETE removes the terminal: hide it from listings immediately and let
  // the sweep timer reap the child and drop the registry entry once the PTY
  // handles are closed. The posted cleanup owns the teardown through the
  // shared_ptr captured above.
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    session->remove_on_close = true;
    if (session->state == "running" || session->state == "starting") {
      session->state = "closing";
    }
  }
  return true;
}

bool TerminalManager::input(const std::string& id, const std::string& bytes,
                            std::string* error) {
  if (bytes.empty()) return true;
  if (bytes.size() > kMaxInput) {
    if (error) *error = "terminal input exceeds 64 KiB";
    return false;
  }
  std::shared_ptr<Impl::Session> session;
  std::shared_ptr<TerminalChildService> child;
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    session = impl_->session_locked(id);
    if (!session) {
      if (error) *error = "terminal not found";
      return false;
    }
    if (session->isolated) {
      if (!session->child) {
        if (error) *error = "terminal is not running";
        return false;
      }
      const auto snapshot = session->child->snapshot();
      if (snapshot.value("state", std::string()) != "running") {
        if (error) *error = "terminal is not running";
        return false;
      }
      child = session->child;
    } else if (session->state != "running") {
      if (error) *error = "terminal is not running";
      return false;
    }
  }
  if (child) return child->input(bytes, error);
  return impl_->post([this, session, bytes] {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
#if defined(_WIN32)
    if (session->state != "running" || session->input_write == nullptr) return;
#else
    if (session->state != "running" || session->poll == nullptr) return;
#endif
    if (session->pending_input.size() + bytes.size() > kMaxInput) return;
    session->pending_input += bytes;
#if defined(_WIN32)
    Impl::pump_input(*session);
#else
    session->poll->start(UV_READABLE | UV_WRITABLE, Impl::on_poll);
#endif
  });
}

bool TerminalManager::resize(const std::string& id, int cols, int rows,
                             std::string* error) {
  const int safe_cols = clamp_int(cols, 20, 500);
  const int safe_rows = clamp_int(rows, 5, 200);
  std::shared_ptr<Impl::Session> session;
  std::shared_ptr<TerminalChildService> child;
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    session = impl_->session_locked(id);
    if (!session) {
      if (error) *error = "terminal not found";
      return false;
    }
    child = session->child;
  }
  if (session->isolated) {
    if (!child) {
      if (error) *error = "terminal is not running";
      return false;
    }
    return child->resize(safe_cols, safe_rows, error);
  }
  return impl_->post([this, session, safe_cols, safe_rows] {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    session->cols = safe_cols;
    session->rows = safe_rows;
#if !defined(_WIN32)
    if (session->fd >= 0) {
      struct winsize ws;
      std::memset(&ws, 0, sizeof(ws));
      ws.ws_col = static_cast<unsigned short>(safe_cols);
      ws.ws_row = static_cast<unsigned short>(safe_rows);
      ioctl(session->fd, TIOCSWINSZ, &ws);
    }
#else
    if (session->hpc != nullptr && conpty_api().resize != nullptr) {
      COORD size;
      size.X = static_cast<SHORT>(safe_cols);
      size.Y = static_cast<SHORT>(safe_rows);
      conpty_api().resize(session->hpc, size);
    }
#endif
  });
}

bool TerminalManager::redraw(const std::string& id, std::string* error) {
  std::shared_ptr<Impl::Session> session;
  std::shared_ptr<TerminalChildService> child;
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    session = impl_->session_locked(id);
    if (!session) {
      if (error) *error = "terminal not found";
      return false;
    }
    child = session->child;
  }
  if (session->isolated) {
    if (!child) {
      if (error) *error = "terminal is not running";
      return false;
    }
    return child->redraw(error);
  }
  return impl_->post([this, session] {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
#if !defined(_WIN32)
    if (session->pid > 0 && session->state == "running") {
      kill(session->pid, SIGWINCH);
    }
#else
    if (session->hpc != nullptr && conpty_api().resize != nullptr &&
        session->state == "running") {
      COORD size;
      size.X = static_cast<SHORT>(session->cols);
      size.Y = static_cast<SHORT>(session->rows + 1);
      conpty_api().resize(session->hpc, size);
      size.Y = static_cast<SHORT>(session->rows);
      conpty_api().resize(session->hpc, size);
    }
#endif
  });
}

nlohmann::json TerminalManager::output(const std::string& id, std::uint64_t after,
                                       int limit) const {
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  const auto session = impl_->session_locked(id);
  if (!session) return {{"ok", false}, {"error", "terminal not found"}};
  if (session->isolated) {
    nlohmann::json out;
    if (session->child) {
      out = session->child->output(after, limit);
    } else {
      // Between a failed restart and the next supervisor attempt there is no
      // child; tell attached clients to reset before the replacement stream.
      out = {{"ok", true},          {"chunks", nlohmann::json::array()},
             {"firstSeq", 0},       {"nextSeq", 0},
             {"truncated", false},  {"missed", after > 0}};
    }
    out["session"] = Impl::snapshot_locked(*session);
    return out;
  }
  const int safe_limit = std::max(1, std::min(limit, 500));
  nlohmann::json chunks = nlohmann::json::array();
  std::uint64_t first_seq = 0;
  for (const auto& chunk : session->chunks) {
    if (chunk.seq <= after) continue;
    if (first_seq == 0) first_seq = chunk.seq;
    chunks.push_back({{"seq", chunk.seq}, {"data", base64_encode(chunk.data)}});
    if (chunks.size() >= static_cast<std::size_t>(safe_limit)) break;
  }
  return {{"ok", true},
          {"session", Impl::snapshot_locked(*session)},
          {"chunks", chunks},
          {"firstSeq", first_seq},
          {"nextSeq", session->next_seq - 1},
          {"truncated", !session->chunks.empty() && session->chunks.front().seq > 1},
          {"missed", after > 0 && !session->chunks.empty() &&
                         after < session->chunks.front().seq - 1}};
}

bool TerminalManager::session_running(const std::string& id) const {
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  const auto session = impl_->session_locked(id);
  if (!session) return false;
  // An isolated child stays alive after its shell exits so the scrollback and
  // exit status remain readable; `exit` must not look like a crash.
  if (session->isolated) return session->child && session->child->running();
  return session->state == "running" || session->state == "starting";
}

bool TerminalManager::stop_runtime(const std::string& id, std::string* error) {
  std::shared_ptr<Impl::Session> session;
  std::shared_ptr<TerminalChildService> child;
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    session = impl_->session_locked(id);
    if (!session) {
      if (error) *error = "terminal not found";
      return false;
    }
    if (!session->isolated) {
      if (error) *error = "terminal is not isolated";
      return false;
    }
    child = session->child;
  }
  if (child) child->stop();
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  if (session->child == child) session->state = "stopped";
  return true;
}

bool TerminalManager::restart(const std::string& id, std::string* error) {
  TerminalConfig config;
  std::shared_ptr<Impl::Session> session;
  std::shared_ptr<TerminalChildService> previous;
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    session = impl_->session_locked(id);
    if (!session) {
      if (error) *error = "terminal not found";
      return false;
    }
    if (!session->isolated) {
      if (error) *error = "terminal is not isolated";
      return false;
    }
    // The supervisor may dispatch twice while a slow spawn is in flight.
    if (session->child && session->child->running()) return true;
    previous = session->child;
    config.name = session->name;
    config.shell = session->shell;
    config.cwd = session->cwd;
    config.cols = session->cols;
    config.rows = session->rows;
    config.isolated = true;
  }
  if (previous) previous->stop();
  auto child = std::make_shared<TerminalChildService>(impl_->executable_path, config);
  std::string child_error;
  if (!child->start(&child_error)) {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    if (session->child == previous) {
      // Drop the dead handle so snapshots report the error from this side
      // instead of merging a stale "stopped" child state.
      session->child.reset();
      session->state = "error";
      session->error =
          child_error.empty() ? "terminal child failed to start" : child_error;
    }
    if (error) {
      *error = child_error.empty() ? "terminal child failed to start" : child_error;
    }
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  const auto snapshot = child->snapshot();
  session->child = std::move(child);
  session->state = snapshot.value("state", std::string("running"));
  session->error.clear();
  session->exited_at.clear();
  session->exit_code = -1;
  return true;
}

void TerminalManager::close_all() {
  std::vector<std::shared_ptr<Impl::Session>> all;
  std::vector<std::shared_ptr<Impl::Session>> isolated;
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    for (const auto& item : impl_->sessions) {
      if (item.second->isolated) {
        isolated.push_back(item.second);
      } else {
        all.push_back(item.second);
      }
    }
  }
  for (const auto& session : isolated) {
    if (session->child) session->child->stop();
  }
  if (!isolated.empty()) {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    for (const auto& session : isolated) impl_->sessions.erase(session->id);
  }
  if (all.empty()) return;
  impl_->post([this, all] {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    for (const auto& session : all) {
      Impl::close_poll(*session);
      Impl::close_fd(session->fd);
#if defined(_WIN32)
      if (session->process != nullptr) {
        TerminateProcess(session->process, 1);
        session->state = "closing";
      }
      if (session->input_write != nullptr) {
          CloseHandle(static_cast<HANDLE>(session->input_write));
          session->input_write = nullptr;
        }
      if (session->hpc != nullptr) {
        if (conpty_api().close != nullptr) conpty_api().close(session->hpc);
        session->hpc = nullptr;
      }
#else
      if (session->pid > 0) {
        kill(session->pid, SIGHUP);
        session->state = "closing";
      }
#endif
    }
  });
}

}  // namespace uvscope
