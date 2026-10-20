#include "terminal_manager.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
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
#include <uvcpp/uvcpp_buf.h>

#include "util.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__linux__)
#include <pty.h>
#elif defined(__APPLE__)
#include <util.h>
#endif
#endif

namespace uvscope {
namespace {

constexpr std::size_t kMaxSessions = 32;
constexpr std::size_t kMaxSessionsPerUser = 4;
constexpr std::size_t kMaxRetainedSessions = 128;
constexpr std::size_t kMaxScrollback = 256 * 1024;
constexpr std::size_t kMaxInput = 64 * 1024;
constexpr int kSweepIntervalMs = 200;

std::string default_shell() {
#if defined(_WIN32)
  return std::string();
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
  (void)shell;
  return false;
#else
  if (shell.empty() || shell.front() != '/') return false;
  return access(shell.c_str(), X_OK) == 0;
#endif
}

bool valid_cwd(const std::string& cwd) {
#if defined(_WIN32)
  (void)cwd;
  return false;
#else
  if (cwd.empty() || cwd.front() != '/') return false;
  struct stat st;
  return stat(cwd.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

std::string default_cwd() {
#if defined(_WIN32)
  return std::string();
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
    std::int64_t owner_id = 0;
    std::string owner_name;
    std::string name;
    std::string shell;
    std::string cwd;
    std::string state = "starting";  // starting | running | closing | exited | error
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
    uv_poll_t* poll = nullptr;
    bool polling = false;
    std::string pending_input;
  };

  static nlohmann::json snapshot_locked(const Session& session);

  struct CreateState {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    std::string error;
  };

  std::thread thread;
  std::mutex ready_mutex;
  std::condition_variable ready_cv;
  bool ready = false;
  std::string start_error;
  bool available = false;

  uvcpp::uvcpp_loop* loop = nullptr;
  uvcpp::uvcpp_async* wake = nullptr;
  uv_timer_t* sweep_timer = nullptr;

  std::mutex command_mutex;
  bool accepting = true;
  std::vector<std::function<void()>> commands;

  mutable std::mutex state_mutex;
  std::map<std::string, std::shared_ptr<Session>> sessions;

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
    return it == sessions.end() ? nullptr : it->second;
  }

  static void append_output(Session& session, const char* data, std::size_t len) {
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

  static void close_fd(int& fd) {
#if !defined(_WIN32)
    if (fd >= 0) ::close(fd);
#endif
    fd = -1;
  }

  static void close_poll(Session& session) {
    if (session.poll == nullptr) return;
    uv_poll_t* handle = session.poll;
    session.poll = nullptr;
    session.polling = false;
    uv_poll_stop(handle);
    handle->data = nullptr;
    uv_close(reinterpret_cast<uv_handle_t*>(handle),
             [](uv_handle_t* raw) { delete reinterpret_cast<uv_poll_t*>(raw); });
  }

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
      uv_poll_start(session.poll, UV_READABLE, on_poll);
    }
#endif
  }

  static void on_poll(uv_poll_t* handle, int status, int events) {
    auto* session = static_cast<Session*>(handle->data);
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
    auto* handle = new uv_poll_t();
    handle->data = &session;
    if (uv_poll_init(reinterpret_cast<uv_loop_t*>(current_loop->get_handle()), handle,
                     session.fd) != 0) {
      delete handle;
      return;
    }
    session.poll = handle;
    session.polling = true;
    uv_poll_start(handle, UV_READABLE, on_poll);
  }

  static thread_local uvcpp::uvcpp_loop* current_loop;

  static void reap(Session& session) {
#if !defined(_WIN32)
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
    std::lock_guard<std::mutex> lock(state_mutex);
    for (auto& item : sessions) {
      Session& session = *item.second;
      if (session.state == "running" || session.state == "closing") reap(session);
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
      sessions.erase(victim);
    }
  }

  static void fork_session(const std::shared_ptr<Session>& session,
                           const std::shared_ptr<CreateState>& state) {
#if defined(_WIN32)
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->error = "terminal PTY is not available on Windows yet";
      state->done = true;
    }
    state->cv.notify_all();
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

    int master = -1;
    const pid_t pid = forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->error = "forkpty failed: " + std::string(std::strerror(errno));
      state->done = true;
      state->cv.notify_all();
      return;
    }
    if (pid == 0) {
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

    auto* timer = new uv_timer_t();
    timer->data = this;
    if (uv_timer_init(reinterpret_cast<uv_loop_t*>(loop->get_handle()), timer) != 0) {
      delete timer;
      delete wake;
      wake = nullptr;
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
    uv_timer_start(timer, [](uv_timer_t* handle) {
      static_cast<Impl*>(handle->data)->sweep();
    }, kSweepIntervalMs, kSweepIntervalMs);

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
#if !defined(_WIN32)
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
      uv_close(reinterpret_cast<uv_handle_t*>(sweep_timer),
               [](uv_handle_t* handle) { delete reinterpret_cast<uv_timer_t*>(handle); });
      sweep_timer = nullptr;
    }
    if (wake != nullptr) {
      delete wake;
      wake = nullptr;
    }
    loop->loop_close();
    delete loop;
    loop = nullptr;
    current_loop = nullptr;
  }
};

thread_local uvcpp::uvcpp_loop* TerminalManager::Impl::current_loop = nullptr;

TerminalManager::TerminalManager() : impl_(new Impl()) {
#if defined(_WIN32)
  impl_->start_error = "terminal PTY is not available on Windows yet";
  return;
#else
  impl_->thread = std::thread([this] { impl_->run_loop(); });
  std::unique_lock<std::mutex> lock(impl_->ready_mutex);
  impl_->ready_cv.wait(lock, [this] { return impl_->ready; });
  if (!impl_->start_error.empty()) impl_->available = false;
#endif
}

TerminalManager::~TerminalManager() {
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
#if !defined(_WIN32)
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
    if (error) *error = "shell must be an absolute executable path";
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
  return {{"id", session.id},
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
          {"bufferedBytes", session.buffered_bytes}};
}

nlohmann::json TerminalManager::sessions(std::int64_t owner_id, bool all) const {
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  nlohmann::json out = nlohmann::json::array();
  for (const auto& item : impl_->sessions) {
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
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    session = impl_->session_locked(id);
  }
  if (!session) {
    if (error) *error = "terminal not found";
    return false;
  }
  if (!impl_->post([this, session] {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        if (session->state == "running" || session->state == "starting") {
          session->state = "closing";
        }
        Impl::close_poll(*session);
        Impl::close_fd(session->fd);
#if !defined(_WIN32)
        if (session->pid > 0) {
          kill(session->pid, SIGHUP);
        }
#endif
      })) {
    if (error) *error = "terminal manager is stopping";
    return false;
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
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    session = impl_->session_locked(id);
    if (!session) {
      if (error) *error = "terminal not found";
      return false;
    }
    if (session->state != "running") {
      if (error) *error = "terminal is not running";
      return false;
    }
  }
  return impl_->post([this, session, bytes] {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    if (session->state != "running" || session->poll == nullptr) return;
    if (session->pending_input.size() + bytes.size() > kMaxInput) return;
    session->pending_input += bytes;
    uv_poll_start(session->poll, UV_READABLE | UV_WRITABLE, Impl::on_poll);
  });
}

bool TerminalManager::resize(const std::string& id, int cols, int rows,
                             std::string* error) {
  const int safe_cols = clamp_int(cols, 20, 500);
  const int safe_rows = clamp_int(rows, 5, 200);
  std::shared_ptr<Impl::Session> session;
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    session = impl_->session_locked(id);
    if (!session) {
      if (error) *error = "terminal not found";
      return false;
    }
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
#endif
  });
}

nlohmann::json TerminalManager::output(const std::string& id, std::uint64_t after,
                                       int limit) const {
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  const auto session = impl_->session_locked(id);
  if (!session) return {{"ok", false}, {"error", "terminal not found"}};
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

void TerminalManager::close_all() {
  std::vector<std::shared_ptr<Impl::Session>> all;
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    for (const auto& item : impl_->sessions) all.push_back(item.second);
  }
  if (all.empty()) return;
  impl_->post([this, all] {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    for (const auto& session : all) {
      Impl::close_poll(*session);
      Impl::close_fd(session->fd);
#if !defined(_WIN32)
      if (session->pid > 0) {
        kill(session->pid, SIGHUP);
        session->state = "closing";
      }
#endif
    }
  });
}

}  // namespace uvscope
