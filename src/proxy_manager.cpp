#include "proxy_manager.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <memory>
#include <regex>
#include <sstream>

#include <uv.h>
#include <ssl/uvcpp_ssl_context.h>
#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>
#include <webapp/uvcpp_web_app.h>

#include "util.h"

namespace uvscope {
namespace {

bool hop_by_hop(const std::string& name) {
  static const char* const names[] = {
      "connection", "keep-alive", "proxy-authenticate", "proxy-authorization",
      "te", "trailer", "transfer-encoding", "upgrade", "content-length"};
  const std::string lowered = lower(name);
  for (const char* candidate : names) {
    if (lowered == candidate) return true;
  }
  return false;
}

std::string iso_time() { return now_iso8601(); }

std::string replace_all(std::string value, const std::string& needle,
                        const std::string& replacement) {
  if (needle.empty()) return value;
  std::size_t pos = 0;
  while ((pos = value.find(needle, pos)) != std::string::npos) {
    value.replace(pos, needle.size(), replacement);
    pos += replacement.size();
  }
  return value;
}

std::string expand_destination(const ProxyRule& rule, const std::string& path,
                               const std::string& query, const std::string& host,
                               const std::string& scheme, const std::string& rest,
                               const std::smatch& match) {
  std::string out = rule.destination;
  out = replace_all(out, "$request_uri", query.empty() ? path : path + "?" + query);
  out = replace_all(out, "$uri", path);
  out = replace_all(out, "$args", query);
  out = replace_all(out, "$host", host);
  out = replace_all(out, "$scheme", scheme);
  out = replace_all(out, "$rest", rest);
  for (int i = 1; i <= 9; ++i) {
    const std::string token = "$" + std::to_string(i);
    const std::string value =
        static_cast<std::size_t>(i) < match.size() ? match[i].str() : std::string();
    out = replace_all(out, token, value);
  }
  // `$args` / `$request_uri` already carry the query; don't append it twice.
  const bool query_in_template = rule.destination.find("$args") != std::string::npos ||
                                 rule.destination.find("$request_uri") != std::string::npos;
  if (rule.preserve_query && !query_in_template && !query.empty()) {
    if (out.find('?') == std::string::npos) {
      out += "?" + query;
    } else if (!out.empty() && out.back() != '?' && out.back() != '&') {
      out += "&" + query;
    } else {
      out += query;
    }
  }
  return out;
}

}  // namespace

struct ProxyManager::Runtime {
  ProxyConfig config;
  std::unique_ptr<uvcpp::uvcpp_web_app> app;
  std::atomic<bool> running{false};
  std::string last_error;
  std::string started_at;
  std::atomic<std::uint64_t> requests{0};
  std::atomic<std::uint64_t> errors{0};
  std::atomic<std::uint64_t> bytes{0};
  std::uint64_t next_log_seq = 1;
  std::deque<nlohmann::json> log_entries;
  mutable std::mutex log_mutex;

  void add_log(const nlohmann::json& entry) {
    std::lock_guard<std::mutex> lock(log_mutex);
    nlohmann::json copy = entry;
    copy["seq"] = next_log_seq++;
    copy["time"] = iso_time();
    log_entries.push_back(copy);
    while (log_entries.size() > 500) log_entries.pop_front();
  }

  void forward(uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp) {
    const auto started = std::chrono::steady_clock::now();
    requests.fetch_add(1);
    const ParsedUrl target = parse_url(config.target);
    if (!target.valid) {
      errors.fetch_add(1);
      resp.status(502).json_str("{\"error\":\"invalid proxy target\"}");
      resp.end();
      add_log({{"method", req.method_name()}, {"path", req.raw_url()}, {"status", 502},
               {"error", "invalid proxy target"}, {"latencyMs", 0}});
      return;
    }

    uvcpp::uvcpp_http_request upstream;
    upstream.method = req.method();
    std::string path = req.raw_path();
    if (path.empty()) path = "/";
    std::string query = req.query_string();
    bool rewritten = false;

    // First enabled matching rule wins. Redirects answer immediately; rewrites
    // replace the upstream path (the target base path then does not apply).
    for (const auto& rule : config.rules) {
      if (!rule.enabled) continue;
      bool matched = false;
      std::string rest;
      std::smatch capture;
      if (rule.match_type == "exact") {
        matched = path == rule.pattern;
      } else if (rule.match_type == "prefix") {
        matched = path.rfind(rule.pattern, 0) == 0;
        if (matched) rest = path.substr(rule.pattern.size());
      } else if (rule.match_type == "regex") {
        try {
          matched = std::regex_search(path, capture, std::regex(rule.pattern));
        } catch (const std::regex_error&) {
          matched = false;
        }
      }
      if (!matched) continue;

      const std::string location = expand_destination(
          rule, path, query, req.host(), config.tls ? "https" : "http", rest, capture);
      if (rule.action == "redirect") {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
        resp.status(rule.status)
            .set_header("location", location)
            .set_header("cache-control", "no-store")
            .set_header("x-uvscope-proxy", config.id);
        resp.end();
        add_log({{"method", req.method_name()}, {"path", req.raw_url()},
                 {"status", rule.status}, {"rule", rule.name.empty() ? rule.id : rule.name},
                 {"action", "redirect"}, {"location", location}, {"latencyMs", elapsed},
                 {"bytes", 0}});
        return;
      }
      // rewrite
      const std::size_t split = location.find('?');
      path = split == std::string::npos ? location : location.substr(0, split);
      query = split == std::string::npos ? std::string() : location.substr(split + 1);
      if (path.empty()) path = "/";
      rewritten = true;
      break;
    }

    if (!rewritten && target.path != "/" && !target.path.empty()) {
      path = join_path(target.path, path);
    }
    if (!query.empty()) path += "?" + query;
    upstream.url = path;

    bool has_host = false;
    for (const auto& header : req.headers()) {
      if (hop_by_hop(header.name)) continue;
      if (lower(header.name) == "host") {
        has_host = true;
        continue;
      }
      upstream.set_header(header.name, header.value);
    }
    if (!has_host) {
      upstream.set_header("host", target.port == 443 ? target.host : target.host + ":" + std::to_string(target.port));
    }
    if (req.peer_ip().size() > 0) upstream.set_header("x-forwarded-for", req.peer_ip());
    upstream.set_header("x-forwarded-host", req.host());
    upstream.set_header("x-forwarded-proto", config.tls ? "https" : "http");
    upstream.set_header("x-uvscope-proxy", config.id);
    upstream.body.clone_data(req.body_data(), req.body_size());

    std::unique_ptr<uvcpp::uvcpp_ssl_context> ssl;
    const bool tls_upstream = target.scheme == "https";
    if (tls_upstream) {
      ssl.reset(new uvcpp::uvcpp_ssl_context(uvcpp::tls_mode::CLIENT,
                                             uvcpp::tls_version::TLS_1_2));
      const std::string ca = find_ca_bundle(executable_dir(nullptr));
      if (!ca.empty()) ssl->load_ca_file(ca);
      ssl->set_verify_mode(uvcpp::tls_verify_mode::PEER_STRICT);
    }

    uvcpp::uvcpp_http_client client;
    if (ssl) client.set_ssl_context(ssl.get());
    int rc = client.connect_wait(target.host.c_str(), target.port, 15000);
    uvcpp::uvcpp_http_response response;
    if (rc == 0) {
      rc = ssl ? client.send_wait_ssl(upstream, response, 30000)
               : client.send_wait_plain(upstream, response, 30000);
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    if (rc != 0) {
      errors.fetch_add(1);
      const std::string message = rc == 0 ? std::string("upstream failed")
                                          : uv_error_string(rc);
      resp.status(502).set_header("content-type", "application/json")
          .json_str(nlohmann::json({{"error", "upstream request failed"},
                                    {"detail", message},
                                    {"proxy", config.name}})
                        .dump());
      resp.end();
      add_log({{"method", req.method_name()}, {"path", req.raw_url()}, {"status", 502},
               {"error", message}, {"latencyMs", elapsed}, {"bytes", 0}});
      return;
    }

    resp.status(static_cast<int>(response.status_code));
    for (const auto& header : response.headers) {
      if (hop_by_hop(header.name)) continue;
      if (lower(header.name) == "set-cookie") {
        resp.add_header(header.name, header.value);
      } else {
        resp.set_header(header.name, header.value);
      }
    }
    const std::string body = response.body.to_string();
    bytes.fetch_add(body.size());
    if (!body.empty()) {
      const std::string ct = response.get_header("content-type");
      if (!ct.empty()) {
        resp.body(body, ct);
      } else {
        resp.body(body);
      }
    }
    resp.set_header("x-uvscope-proxy", config.id);
    resp.end();
    add_log({{"method", req.method_name()}, {"path", req.raw_url()},
             {"status", static_cast<int>(response.status_code)}, {"latencyMs", elapsed},
             {"bytes", body.size()}});
  }

  ~Runtime() {
    if (app) {
      app->stop();
      app->join();
    }
  }
};

ProxyManager::ProxyManager(Store& store) : store_(store) { auto_start(); }

ProxyManager::~ProxyManager() { stop_all(); }

void ProxyManager::auto_start() {
  for (const auto& config : store_.proxies()) {
    if (!config.auto_start) continue;
    std::string error;
    if (!start(config.id, &error)) {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto& runtime : runtimes_) {
        if (runtime->config.id == config.id) {
          runtime->last_error = error;
          runtime->add_log({{"method", "SYSTEM"}, {"path", "/"}, {"status", 0},
                            {"error", "auto-start failed: " + error}, {"latencyMs", 0}});
        }
      }
    }
  }
}

std::shared_ptr<ProxyManager::Runtime> ProxyManager::find(const std::string& id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& runtime : runtimes_) {
    if (runtime->config.id == id) return runtime;
  }
  return nullptr;
}

bool ProxyManager::start(const std::string& id, std::string* error) {
  const auto config = store_.proxy(id);
  if (!config) {
    if (error) *error = "proxy not found";
    return false;
  }
  std::shared_ptr<Runtime> runtime;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& existing : runtimes_) {
      if (existing->config.id == id) {
        runtime = existing;
        break;
      }
    }
    if (!runtime) {
      runtime = std::make_shared<Runtime>();
      runtime->config = *config;
      runtimes_.push_back(runtime);
    } else if (runtime->running.load()) {
      if (error) *error = "proxy is already running";
      return false;
    } else {
      runtime->config = *config;
      runtime->last_error.clear();
    }
  }

  if (runtime->app) {
    runtime->app->stop();
    runtime->app->join();
    runtime->app.reset();
  }

  auto app = std::unique_ptr<uvcpp::uvcpp_web_app>(new uvcpp::uvcpp_web_app());
  app->set_host(config->listen_host)
      .set_port(config->listen_port)
      .set_access_log(false)
      .set_shutdown_grace_ms(500)
      .set_http2_enabled(false);
  if (config->tls) {
    if (config->cert_path.empty() || config->key_path.empty()) {
      const std::string detail = "TLS proxy requires a certificate and private key path";
      runtime->last_error = detail;
      runtime->add_log({{"method", "SYSTEM"}, {"path", "/"}, {"status", 0},
                        {"error", detail}, {"latencyMs", 0}});
      if (error) *error = detail;
      return false;
    }
    app->enable_ssl(config->cert_path, config->key_path);
    if (!app->ssl_error().empty()) {
      const std::string detail = "failed to load TLS certificate: " + app->ssl_error();
      runtime->last_error = detail;
      runtime->add_log({{"method", "SYSTEM"}, {"path", "/"}, {"status", 0},
                        {"error", detail}, {"latencyMs", 0}});
      if (error) *error = detail;
      return false;
    }
  }
  Runtime* raw = runtime.get();
  const auto handler = [raw](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                             uvcpp::uvcpp_web_next) { raw->forward(req, resp); };
  app->any("/", handler);
  app->any("/*path", handler);
  const int rc = app->start();
  if (rc != 0) {
    const std::string detail = "bind/listen failed: " + uv_error_string(rc) + " on " +
                               config->listen_host + ":" + std::to_string(config->listen_port);
    runtime->last_error = detail;
    if (error) *error = detail;
    runtime->add_log({{"method", "SYSTEM"}, {"path", "/"}, {"status", 0},
                      {"error", detail}, {"latencyMs", 0}});
    return false;
  }

  runtime->app = std::move(app);
  runtime->running.store(true);
  runtime->started_at = now_iso8601();
  runtime->last_error.clear();
  runtime->add_log({{"method", "SYSTEM"}, {"path", "/"}, {"status", 0},
                    {"message", "proxy started on " + config->listen_host + ":" +
                                    std::to_string(config->listen_port)},
                    {"latencyMs", 0}});
  return true;
}

bool ProxyManager::stop(const std::string& id, std::string* error) {
  const auto runtime = find(id);
  if (!runtime) {
    if (error) *error = "proxy not found";
    return false;
  }
  if (runtime->app) {
    runtime->app->stop();
    runtime->app->join();
    runtime->app.reset();
  }
  runtime->running.store(false);
  runtime->add_log({{"method", "SYSTEM"}, {"path", "/"}, {"status", 0},
                    {"message", "proxy stopped"}, {"latencyMs", 0}});
  return true;
}

void ProxyManager::stop_all() {
  std::vector<std::shared_ptr<Runtime>> runtimes;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    runtimes = runtimes_;
  }
  for (const auto& runtime : runtimes) {
    if (runtime->app) {
      runtime->app->stop();
      runtime->app->join();
      runtime->app.reset();
    }
    runtime->running.store(false);
  }
}

bool ProxyManager::reload(const ProxyConfig& config, std::string* error) {
  if (config.id.empty()) {
    if (error) *error = "proxy id is required";
    return false;
  }
  if (!store_.save_proxy(config, error)) return false;
  if (config.listen_port < 1 || config.listen_port > 65535) {
    if (error) *error = "listen port must be between 1 and 65535";
    return false;
  }
  if (!parse_url(config.target).valid) {
    if (error) *error = "target must be a valid http(s) URL";
    return false;
  }
  const auto runtime = find(config.id);
  if (runtime && runtime->running.load()) {
    if (!stop(config.id, error)) return false;
    return start(config.id, error);
  }
  return true;
}

bool ProxyManager::is_running(const std::string& id) const {
  const auto runtime = find(id);
  return runtime && runtime->running.load();
}

nlohmann::json ProxyManager::snapshot() const {
  nlohmann::json out = nlohmann::json::array();
  std::vector<std::shared_ptr<Runtime>> runtimes;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    runtimes = runtimes_;
  }
  for (const auto& runtime : runtimes) {
    out.push_back({{"id", runtime->config.id},
                   {"name", runtime->config.name},
                   {"listenHost", runtime->config.listen_host},
                   {"listenPort", runtime->config.listen_port},
                   {"target", runtime->config.target},
                   {"autoStart", runtime->config.auto_start},
                   {"tls", runtime->config.tls},
                   {"running", runtime->running.load()},
                   {"startedAt", runtime->started_at},
                   {"lastError", runtime->last_error},
                   {"requests", runtime->requests.load()},
                   {"errors", runtime->errors.load()},
                   {"bytes", runtime->bytes.load()}});
  }
  return out;
}

nlohmann::json ProxyManager::logs(const std::string& id, std::uint64_t after, int limit) const {
  const auto runtime = find(id);
  nlohmann::json out = nlohmann::json::array();
  if (!runtime) return out;
  const int safe_limit = std::max(1, std::min(limit, 500));
  std::lock_guard<std::mutex> lock(runtime->log_mutex);
  int emitted = 0;
  for (auto it = runtime->log_entries.rbegin();
       it != runtime->log_entries.rend() && emitted < safe_limit; ++it) {
    if (it->value("seq", std::uint64_t(0)) <= after) continue;
    out.push_back(*it);
    ++emitted;
  }
  std::reverse(out.begin(), out.end());
  return out;
}

std::uint64_t ProxyManager::total_requests() const {
  std::uint64_t total = 0;
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& runtime : runtimes_) total += runtime->requests.load();
  return total;
}

std::uint64_t ProxyManager::total_errors() const {
  std::uint64_t total = 0;
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& runtime : runtimes_) total += runtime->errors.load();
  return total;
}

std::size_t ProxyManager::active_count() const {
  std::size_t count = 0;
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& runtime : runtimes_) {
    if (runtime->running.load()) ++count;
  }
  return count;
}

}  // namespace uvscope
