#include "download_manager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include <uv.h>
#include <ssl/uvcpp_ssl_context.h>
#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_common.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>

#include "util.h"

namespace uvscope {
namespace {

constexpr std::int64_t kMaxDownloadBytes = 512LL * 1024 * 1024;
constexpr int kJobTtlSeconds = 600;
constexpr std::size_t kMaxJobsPerUser = 4;
constexpr std::size_t kMaxJobs = 32;

std::string default_user_agent() {
#if defined(_WIN32)
  const std::string ua =
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";
#elif defined(__APPLE__)
  const std::string ua =
      "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";
#else
  const std::string ua =
      "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";
#endif
  return ua + " uvscope/" UVSCOPE_VERSION;
}

uvcpp::http_method method_of(const std::string& value) {
  const std::string method = lower(value.empty() ? "GET" : value);
  using M = uvcpp::http_method;
  if (method == "post") return M::HTTP_POST;
  if (method == "put") return M::HTTP_PUT;
  if (method == "delete") return M::HTTP_DELETE;
  if (method == "patch") return M::HTTP_PATCH;
  if (method == "head") return M::HTTP_HEAD;
  if (method == "options") return M::HTTP_OPTIONS;
  return M::HTTP_GET;
}

std::string safe_filename(std::string value) {
  const std::size_t slash = value.find_last_of('/');
  if (slash != std::string::npos) value = value.substr(slash + 1);
  const std::size_t query = value.find_first_of("?#");
  if (query != std::string::npos) value = value.substr(0, query);
  std::string out;
  for (char c : value) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (ok) out.push_back(c);
  }
  while (!out.empty() && out.front() == '.') out.erase(out.begin());
  if (out.empty()) out = "download.bin";
  if (out.size() > 100) out.resize(100);
  return out;
}

// Parses "bytes START-END/TOTAL" (TOTAL may be "*"). Returns false for a
// malformed or non-byte unit.
bool parse_content_range(const std::string& value, std::int64_t* start,
                         std::int64_t* end, std::int64_t* total) {
  const std::string text = lower(trim(value));
  if (text.rfind("bytes ", 0) != 0) return false;
  const std::size_t dash = text.find('-', 6);
  const std::size_t slash = text.find('/', dash == std::string::npos ? 6 : dash + 1);
  if (dash == std::string::npos || slash == std::string::npos) return false;
  try {
    const std::string start_text = trim(text.substr(6, dash - 6));
    const std::string end_text = trim(text.substr(dash + 1, slash - dash - 1));
    const std::string total_text = trim(text.substr(slash + 1));
    if (start_text.empty() || end_text.empty() || total_text.empty()) return false;
    *start = std::stoll(start_text);
    *end = std::stoll(end_text);
    *total = total_text == "*" ? -1 : std::stoll(total_text);
  } catch (...) {
    return false;
  }
  return *start >= 0 && *end >= *start;
}

std::string disposition_filename(const std::string& value) {
  const std::string key = "filename=";
  const std::size_t at = lower(value).find(key);
  if (at == std::string::npos) return {};
  std::string name = value.substr(at + key.size());
  const std::size_t semi = name.find(';');
  if (semi != std::string::npos) name.resize(semi);
  name = trim(name);
  if (name.size() >= 2 && (name.front() == '"' || name.front() == '\'')) {
    const char quote = name.front();
    if (name.back() == quote) name = name.substr(1, name.size() - 2);
  }
  return safe_filename(name);
}

}  // namespace

struct DownloadManager::Job {
  std::string id;
  std::string url;
  std::filesystem::path file_path;
  std::int64_t owner_id = 0;
  std::thread thread;

  mutable std::mutex mutex;
  std::string state = "running";  // running | ready | error | cancelled
  std::string error;
  std::string content_type = "application/octet-stream";
  std::string filename = "download.bin";
  std::string started_at;
  std::string finished_at;
  int status_code = 0;
  std::int64_t bytes = 0;
  std::int64_t total = -1;
  int attempts = 0;
  HttpProbeInput input;
  std::chrono::steady_clock::time_point finished_wall;

  std::atomic<bool> cancel_requested{false};
  // Set by the worker once the client loop exists; cancel() posts through it.
  std::atomic<uv_async_t*> cancel_async{nullptr};
};

namespace {

struct RunState {
  std::shared_ptr<DownloadManager::Job> job;
  std::unique_ptr<uvcpp::uvcpp_http_client> client;
  FILE* file = nullptr;
  bool failed = false;
  bool ended = false;
  std::string error;
};

FILE* open_temp_file(const std::filesystem::path& path, bool append) {
#if defined(_WIN32)
  return _wfopen(path.wstring().c_str(), append ? L"ab" : L"wb");
#else
  return std::fopen(path.c_str(), append ? "ab" : "wb");
#endif
}

void set_terminal(const std::shared_ptr<DownloadManager::Job>& job,
                  const std::string& state, const std::string& error) {
  std::lock_guard<std::mutex> lock(job->mutex);
  if (job->state == "running") {
    job->state = state;
    job->error = error;
  }
}

void fail(RunState* state, const std::string& error) {
  if (!state->failed) {
    state->failed = true;
    state->error = error;
  }
  if (state->client) state->client->cancel_stream();
}

void on_cancel(uv_async_t* handle) {
  auto* state = static_cast<RunState*>(handle->data);
  if (state == nullptr || state->client == nullptr) return;
  set_terminal(state->job, "cancelled", "download cancelled");
  state->client->cancel_stream();
  state->client->stop();
}

void on_timeout(uv_timer_t* timer) {
  auto* state = static_cast<RunState*>(timer->data);
  if (state == nullptr || state->client == nullptr) return;
  // Do not overwrite a cancel that arrived first.
  {
    std::lock_guard<std::mutex> lock(state->job->mutex);
    if (state->job->state == "running") {
      state->job->state = "error";
      state->job->error = "download timed out";
    }
  }
  state->client->cancel_stream();
  state->client->stop();
}

void run_job(std::shared_ptr<DownloadManager::Job> job, std::int64_t resume_from) {
  const HttpProbeInput& input = job->input;
  const bool resuming = resume_from > 0;
  RunState state;
  state.job = job;
  state.client.reset(new uvcpp::uvcpp_http_client());
  uvcpp::uvcpp_http_client& client = *state.client;
  {
    std::lock_guard<std::mutex> lock(job->mutex);
    job->bytes = resume_from;
    job->status_code = 0;
  }

  const ParsedUrl url = parse_url(job->url);
  if (!url.valid || (url.scheme != "http" && url.scheme != "https")) {
    set_terminal(job, "error", url.valid ? "only http and https URLs can be downloaded"
                                         : "invalid URL: " + url.error);
    return;
  }

  uvcpp::uvcpp_http_request req;
  req.method = method_of(input.method);
  req.url = url_path_with_query(url);
  bool has_host = false;
  bool has_ua = false;
  for (const auto& header : input.headers) {
    const std::string name = lower(trim(header.name));
    if (name.empty()) continue;
    if (name == "content-length" || name == "connection" || name == "transfer-encoding") {
      continue;
    }
    // A resume owns the Range header; stale user ranges would conflict.
    if (resuming && (name == "range" || name == "if-range")) continue;
    if (name == "host") has_host = true;
    if (name == "user-agent") has_ua = true;
    req.set_header(header.name, header.value);
  }
  if (!has_host) {
    req.set_header("host", url.port == (url.scheme == "https" ? 443 : 80)
                               ? url.host
                               : url.host + ":" + std::to_string(url.port));
  }
  if (!has_ua) req.set_header("user-agent", default_user_agent());
  if (resuming) {
    req.set_header("range", "bytes=" + std::to_string(resume_from) + "-");
  }
  req.body.clone_data(input.body.data(), input.body.size());

  uvcpp::uvcpp_ssl_context ssl(uvcpp::tls_mode::CLIENT, uvcpp::tls_version::TLS_1_2);
  if (url.scheme == "https") {
    const std::string ca = find_ca_bundle(executable_dir(nullptr));
    if (!ca.empty()) ssl.load_ca_file(ca);
    ssl.set_verify_mode(input.insecure ? uvcpp::tls_verify_mode::NONE
                                       : uvcpp::tls_verify_mode::PEER_STRICT);
    client.set_ssl_context(&ssl);
  }
  if (input.proxy.enabled()) {
    std::string proxy_error;
    if (!input.proxy.valid(&proxy_error)) {
      set_terminal(job, "error", proxy_error);
      return;
    }
    const int rc = client.set_socks5_proxy(input.proxy.host, input.proxy.port,
                                           input.proxy.username, input.proxy.password);
    if (rc != 0) {
      set_terminal(job, "error", "invalid SOCKS5 proxy: " + uv_error_string(rc));
      return;
    }
  }

  state.file = open_temp_file(job->file_path, resuming);
  if (state.file == nullptr) {
    set_terminal(job, "error", "could not create the download file");
    return;
  }
  {
    std::lock_guard<std::mutex> lock(job->mutex);
    job->filename = safe_filename(url.path);
  }

  uvcpp::uvcpp_loop* loop = client.get_tcp_client()->get_loop();
  uv_loop_t* raw_loop =
      loop == nullptr ? nullptr : reinterpret_cast<uv_loop_t*>(loop->get_handle());
  auto* cancel_handle = new uv_async_t();
  auto* timer = new uv_timer_t();
  if (raw_loop == nullptr || uv_async_init(raw_loop, cancel_handle, on_cancel) != 0 ||
      uv_timer_init(raw_loop, timer) != 0) {
    delete cancel_handle;
    delete timer;
    fail(&state, "could not start the download loop");
    set_terminal(job, "error", state.error);
    std::fclose(state.file);
    state.file = nullptr;
    return;
  }
  cancel_handle->data = &state;
  timer->data = &state;
  const int timeout_ms = std::max(1000, std::min(input.timeout_ms, 600000));
  uv_timer_start(timer, on_timeout, static_cast<std::uint64_t>(timeout_ms), 0);
  job->cancel_async.store(cancel_handle);
  if (job->cancel_requested.load()) {
    set_terminal(job, "cancelled", "download cancelled");
    client.stop();
  }

  const int connect_rc = client.connect(
      url.host.c_str(), url.port,
      [&state, &client, &req, resuming, resume_from](int status) {
        if (status != 0) {
          fail(&state, "connect failed: " + uv_error_string(status));
          client.stop();
          return;
        }
        const int rc = client.send_stream(
            req,
            [&state, resuming, resume_from](
                const uvcpp::uvcpp_http_response& head) {
              bool restart_from_zero = false;
              std::string range_error;
              {
                const int status_code = static_cast<int>(head.status_code);
                std::lock_guard<std::mutex> lock(state.job->mutex);
                state.job->status_code = status_code;
                const std::string type =
                    uvcpp::http_get_header(head.headers, "content-type");
                if (!type.empty()) state.job->content_type = type;
                const std::string disposition =
                    uvcpp::http_get_header(head.headers, "content-disposition");
                const std::string named = disposition_filename(disposition);
                if (!named.empty()) state.job->filename = named;
                const std::string length =
                    uvcpp::http_get_header(head.headers, "content-length");
                std::int64_t content_length = -1;
                if (!length.empty()) {
                  try {
                    content_length = std::stoll(length);
                  } catch (...) {
                    content_length = -1;
                  }
                }
                if (!resuming) {
                  if (content_length >= 0) state.job->total = content_length;
                } else if (status_code == 206) {
                  std::int64_t start = -1;
                  std::int64_t end = -1;
                  std::int64_t total = -1;
                  const std::string range =
                      uvcpp::http_get_header(head.headers, "content-range");
                  if (!parse_content_range(range, &start, &end, &total) ||
                      start != resume_from) {
                    range_error =
                        "server returned an invalid Content-Range for the resume";
                  } else {
                    state.job->total = total;
                  }
                } else if (status_code == 200) {
                  // The server ignored Range: keep the response but restart
                  // from byte zero so the staged file stays consistent.
                  restart_from_zero = true;
                  state.job->total = content_length;
                } else if (status_code == 416) {
                  range_error = "server cannot resume from byte " +
                                std::to_string(resume_from);
                } else if (status_code >= 400) {
                  range_error = "upstream returned status " +
                                std::to_string(status_code);
                } else {
                  range_error = "unexpected status for a ranged resume: " +
                                std::to_string(status_code);
                }
              }
              if (restart_from_zero) {
                if (state.file != nullptr) {
                  std::fflush(state.file);
                  std::fclose(state.file);
                  state.file = nullptr;
                }
                state.file = open_temp_file(state.job->file_path, false);
                if (state.file == nullptr) {
                  fail(&state, "could not restart the download file");
                  return;
                }
                std::lock_guard<std::mutex> lock(state.job->mutex);
                state.job->bytes = 0;
              }
              if (!range_error.empty()) fail(&state, range_error);
            },
            [&state](const char* data, std::size_t len) -> bool {
              if (state.job->cancel_requested.load()) {
                set_terminal(state.job, "cancelled", "download cancelled");
                return false;
              }
              if (state.job->bytes + static_cast<std::int64_t>(len) > kMaxDownloadBytes) {
                fail(&state, "response exceeds the 512 MiB download limit");
                // cancel_stream() already finished the stream; do not request a
                // pause that nothing will ever resume.
                return true;
              }
              if (state.file == nullptr ||
                  std::fwrite(data, 1, len, state.file) != len) {
                fail(&state, "could not write the download file");
                return true;
              }
              {
                std::lock_guard<std::mutex> lock(state.job->mutex);
                state.job->bytes += static_cast<std::int64_t>(len);
              }
              return true;
            },
            [&state, &client](int status) {
              state.ended = true;
              if (status != 0) {
                {
                  std::lock_guard<std::mutex> lock(state.job->mutex);
                  if (state.job->state == "running") {
                    state.job->state = "error";
                    state.job->error = state.error.empty()
                                           ? "stream ended: " + uv_error_string(status)
                                           : state.error;
                  }
                }
                state.failed = true;
              }
              client.stop();
            });
        if (rc != 0) {
          fail(&state, "request failed: " + uv_error_string(rc));
          client.stop();
        }
      });
  if (connect_rc != 0) {
    fail(&state, "connect failed: " + uv_error_string(connect_rc));
    client.stop();
  }

  if (!job->cancel_requested.load() || job->state == "running") {
    client.run(UV_RUN_DEFAULT);
  }
  job->cancel_async.store(nullptr);
  uv_timer_stop(timer);
  uv_close(reinterpret_cast<uv_handle_t*>(timer), [](uv_handle_t* handle) {
    delete reinterpret_cast<uv_timer_t*>(handle);
  });
  uv_close(reinterpret_cast<uv_handle_t*>(cancel_handle), [](uv_handle_t* handle) {
    delete reinterpret_cast<uv_async_t*>(handle);
  });
  client.run(UV_RUN_NOWAIT);

  if (state.file != nullptr) {
    std::fflush(state.file);
    std::fclose(state.file);
    state.file = nullptr;
  }

  std::string state_out = "ready";
  std::string error;
  bool keep_partial = false;
  {
    std::lock_guard<std::mutex> lock(job->mutex);
    if (job->state != "running") {
      state_out = job->state;
      error = job->error;
    } else if (state.failed || !state.ended) {
      state_out = "error";
      error = state.error.empty() ? "download did not complete" : state.error;
    } else if (job->total >= 0 && job->bytes != job->total) {
      // A clean socket close can still be short when Content-Length is known.
      state_out = "error";
      error = "download ended early (" + std::to_string(job->bytes) + " of " +
              std::to_string(job->total) + " bytes)";
    }
    job->state = state_out;
    job->error = error;
    job->finished_at = now_iso8601();
    job->finished_wall = std::chrono::steady_clock::now();
    keep_partial = job->bytes > 0;
  }
  // Keep partial data so an errored or cancelled job can be resumed; complete
  // and empty results have nothing worth retaining.
  if (state_out != "ready" && !keep_partial) {
    std::error_code ec;
    std::filesystem::remove(job->file_path, ec);
  }
}

}  // namespace

DownloadManager::DownloadManager(std::filesystem::path data_dir)
    : dir_(std::move(data_dir) / "downloads") {
  std::error_code ec;
  std::filesystem::create_directories(dir_, ec);
  // Jobs live in memory only, so every file left here belongs to a previous
  // daemon run and has no token that can reach it.
  for (std::filesystem::directory_iterator it(dir_, ec), end; !ec && it != end;
       it.increment(ec)) {
    if (it->is_regular_file(ec)) std::filesystem::remove(it->path(), ec);
  }
}

DownloadManager::~DownloadManager() {
  std::vector<std::shared_ptr<Job>> jobs;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& item : jobs_) jobs.push_back(item.second);
  }
  for (auto& job : jobs) {
    job->cancel_requested.store(true);
    if (uv_async_t* handle = job->cancel_async.load()) uv_async_send(handle);
  }
  for (auto& job : jobs) {
    if (job->thread.joinable()) job->thread.join();
  }
}

std::shared_ptr<DownloadManager::Job> DownloadManager::find(
    const std::string& id) const {
  const auto it = jobs_.find(id);
  return it == jobs_.end() ? nullptr : it->second;
}

void DownloadManager::prune_locked() {
  const auto now = std::chrono::steady_clock::now();
  for (auto it = jobs_.begin(); it != jobs_.end();) {
    const auto& job = it->second;
    std::string state;
    std::chrono::steady_clock::time_point finished{};
    {
      std::lock_guard<std::mutex> lock(job->mutex);
      state = job->state;
      finished = job->finished_wall;
    }
    const bool terminal = state != "running";
    if (terminal) {
      if (finished.time_since_epoch().count() == 0 ||
          now - finished > std::chrono::seconds(kJobTtlSeconds)) {
        if (job->thread.joinable()) job->thread.join();
        std::error_code ec;
        std::filesystem::remove(job->file_path, ec);
        it = jobs_.erase(it);
        continue;
      }
    }
    ++it;
  }
}

std::string DownloadManager::start(const HttpProbeInput& input,
                                   std::int64_t owner_id, std::string* error) {
  const ParsedUrl url = parse_url(input.url);
  if (!url.valid) {
    if (error) *error = url.error.empty() ? "invalid URL" : url.error;
    return {};
  }
  if (url.scheme != "http" && url.scheme != "https") {
    if (error) *error = "only http and https URLs can be downloaded";
    return {};
  }
  auto job = std::make_shared<Job>();
  job->id = random_hex(16);
  job->url = input.url;
  job->owner_id = owner_id;
  job->file_path = dir_ / (job->id + ".bin");
  job->started_at = now_iso8601();
  job->input = input;
  job->attempts = 1;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    prune_locked();
    std::size_t mine = 0;
    for (const auto& item : jobs_) {
      if (item.second->owner_id != owner_id) continue;
      std::lock_guard<std::mutex> item_lock(item.second->mutex);
      if (item.second->state == "running") ++mine;
    }
    if (jobs_.size() >= kMaxJobs) {
      if (error) *error = "too many downloads are retained";
      return {};
    }
    if (mine >= kMaxJobsPerUser) {
      if (error) *error = "too many running downloads for this user";
      return {};
    }
    jobs_[job->id] = job;
  }
  job->thread = std::thread(run_job, job, 0);
  return job->id;
}

bool DownloadManager::resume(const std::string& id, std::int64_t owner_id,
                             bool is_admin, std::string* error) {
  std::shared_ptr<Job> job;
  std::int64_t resume_from = 0;
  bool start_thread = true;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    job = find(id);
    if (!job || (!is_admin && job->owner_id != owner_id)) {
      if (error) *error = "download not found";
      return false;
    }
    {
      std::lock_guard<std::mutex> item_lock(job->mutex);
      if (job->state == "running") {
        if (error) *error = "download is still running";
        return false;
      }
      if (job->state != "error") {
        if (error) *error = "download is not in an error state";
        return false;
      }
      std::error_code ec;
      const auto size = std::filesystem::file_size(job->file_path, ec);
      if (ec) {
        if (error) *error = "partial download file is missing";
        return false;
      }
      resume_from = static_cast<std::int64_t>(size);
      if (resume_from <= 0) {
        if (error) *error = "download has no partial data to resume";
        return false;
      }
      job->bytes = resume_from;
      if (job->total >= 0 && resume_from >= job->total) {
        // The bytes already on disk are the whole response.
        job->state = "ready";
        job->error.clear();
        job->finished_at = now_iso8601();
        job->finished_wall = std::chrono::steady_clock::now();
        start_thread = false;
      } else {
        job->state = "running";
        job->error.clear();
        job->finished_at.clear();
        job->finished_wall = {};
        job->status_code = 0;
        job->attempts += 1;
        job->cancel_requested.store(false);
      }
    }
  }
  if (!start_thread) return true;
  if (job->thread.joinable()) job->thread.join();
  job->thread = std::thread(run_job, job, resume_from);
  return true;
}

bool DownloadManager::snapshot(const std::string& id, std::int64_t owner_id,
                               bool is_admin, DownloadSnapshot* out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto job = find(id);
  if (!job) return false;
  if (!is_admin && job->owner_id != owner_id) return false;
  std::lock_guard<std::mutex> item_lock(job->mutex);
  if (out) {
    out->id = job->id;
    out->state = job->state;
    out->error = job->error;
    out->url = "/download/" + job->id;
    out->contentType = job->content_type;
    out->filename = job->filename;
    out->startedAt = job->started_at;
    out->finishedAt = job->finished_at;
    out->path = job->file_path;
    out->bytes = job->bytes;
    out->totalBytes = job->total;
    out->statusCode = job->status_code;
    out->resumable = job->state == "error" && job->bytes > 0;
    out->attempts = job->attempts;
  }
  return true;
}

bool DownloadManager::cancel(const std::string& id, std::int64_t owner_id,
                             bool is_admin, std::string* error) {
  std::shared_ptr<Job> job;
  bool terminal = false;
  std::filesystem::path remove_path;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    job = find(id);
    if (!job || (!is_admin && job->owner_id != owner_id)) {
      if (error) *error = "download not found";
      return false;
    }
    job->cancel_requested.store(true);
    {
      std::lock_guard<std::mutex> item_lock(job->mutex);
      if (job->state == "running") {
        job->state = "cancelled";
        job->error = "download cancelled";
        job->finished_at = now_iso8601();
        job->finished_wall = std::chrono::steady_clock::now();
      } else {
        // Already finished: delete the staged file and forget the job now.
        terminal = true;
        remove_path = job->file_path;
      }
    }
    if (terminal) {
      if (job->thread.joinable()) job->thread.join();
      jobs_.erase(id);
    }
  }
  if (terminal) {
    std::error_code ec;
    std::filesystem::remove(remove_path, ec);
    return true;
  }
  if (uv_async_t* handle = job->cancel_async.load()) uv_async_send(handle);
  return true;
}

std::optional<DownloadSnapshot> DownloadManager::file(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto job = find(id);
  if (!job) return std::nullopt;
  std::lock_guard<std::mutex> item_lock(job->mutex);
  if (job->state != "ready") return std::nullopt;
  DownloadSnapshot out;
  out.id = job->id;
  out.state = job->state;
  out.error = job->error;
  out.url = "/download/" + job->id;
  out.contentType = job->content_type;
  out.filename = job->filename;
  out.startedAt = job->started_at;
  out.finishedAt = job->finished_at;
  out.path = job->file_path;
  out.bytes = job->bytes;
  out.totalBytes = job->total;
  out.statusCode = job->status_code;
  out.resumable = job->state == "error" && job->bytes > 0;
  out.attempts = job->attempts;
  return out;
}

nlohmann::json download_json(const DownloadSnapshot& snapshot) {
  return {{"id", snapshot.id},
          {"state", snapshot.state},
          {"error", snapshot.error},
          {"url", snapshot.url},
          {"contentType", snapshot.contentType},
          {"filename", snapshot.filename},
          {"startedAt", snapshot.startedAt},
          {"finishedAt", snapshot.finishedAt},
          {"bytes", snapshot.bytes},
          {"totalBytes", snapshot.totalBytes},
          {"statusCode", snapshot.statusCode},
          {"resumable", snapshot.resumable},
          {"attempts", snapshot.attempts}};
}

}  // namespace uvscope
