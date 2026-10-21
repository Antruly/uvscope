#include "proxy_manager.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <memory>
#include <random>
#include <condition_variable>
#include <map>
#include <regex>
#include <sstream>
#include <thread>

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

struct ProxyCandidate {
  std::string url;
  ParsedUrl parsed;
  int weight = 1;
};

int pick_candidate(std::mt19937& rng, const std::vector<ProxyCandidate>& candidates,
                   std::vector<bool>& tried) {
  int total = 0;
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    if (!tried[i]) total += candidates[i].weight;
  }
  if (total <= 0) {
    // Every upstream has been tried once (or all weights are zero): allow a
    // second pass over the list instead of failing without an attempt.
    for (std::size_t i = 0; i < candidates.size(); ++i) {
      tried[i] = false;
      total += candidates[i].weight;
    }
  }
  if (total <= 0) return -1;
  std::uniform_int_distribution<int> dist(1, total);
  int roll = dist(rng);
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    if (tried[i]) continue;
    roll -= candidates[i].weight;
    if (roll <= 0) return static_cast<int>(i);
  }
  return -1;
}

bool method_matches(const std::string& list, const std::string& method) {
  const std::string wanted = lower(trim(method));
  std::size_t start = 0;
  while (start <= list.size()) {
    const std::size_t comma = list.find(',', start);
    const std::string token = lower(trim(list.substr(
        start, comma == std::string::npos ? std::string::npos : comma - start)));
    if (token == wanted) return true;
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return false;
}

std::string host_without_port(std::string host) {
  if (!host.empty() && host.front() == '[') {
    const std::size_t close = host.find(']');
    if (close != std::string::npos) return host.substr(0, close + 1);
  }
  const std::size_t colon = host.rfind(':');
  if (colon != std::string::npos && host.find(':') == colon) host.resize(colon);
  return host;
}

bool host_matches(const std::string& pattern, const std::string& host) {
  const std::string expected = lower(trim(pattern));
  std::string actual = lower(trim(host));
  if (expected.find(':') == std::string::npos) actual = host_without_port(actual);
  if (expected.rfind("*.", 0) == 0) {
    const std::string suffix = expected.substr(1);
    return actual.size() > suffix.size() &&
           actual.compare(actual.size() - suffix.size(), suffix.size(), suffix) == 0;
  }
  return expected == actual;
}

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
  std::mt19937 rng{std::random_device{}()};
  struct Sample {
    std::chrono::steady_clock::time_point at;
    int status = 0;
    double latency_ms = 0;
  };
  mutable std::mutex metrics_mutex;
  std::deque<Sample> samples;

  struct HealthState {
    bool healthy = true;
    bool checked = false;
    long long latency_ms = 0;
    std::string error;
    std::string checked_at;
  };
  mutable std::mutex health_mutex;
  std::map<std::string, HealthState> health;
  std::thread health_thread;
  std::mutex health_wait_mutex;
  std::condition_variable health_cv;
  std::atomic<bool> stop_health{false};

  bool is_healthy(const std::string& url) const {
    std::lock_guard<std::mutex> lock(health_mutex);
    const auto it = health.find(url);
    if (it == health.end() || !it->second.checked) return true;
    return it->second.healthy;
  }

  nlohmann::json health_json() const {
    std::lock_guard<std::mutex> lock(health_mutex);
    nlohmann::json out = nlohmann::json::array();
    std::vector<std::string> urls;
    if (config.upstreams.empty()) urls.push_back(config.target);
    else {
      for (const auto& upstream : config.upstreams) urls.push_back(upstream.url);
    }
    for (const auto& url : urls) {
      const auto it = health.find(url);
      if (it == health.end()) {
        out.push_back({{"url", url}, {"healthy", true}, {"checked", false},
                       {"latencyMs", 0}, {"checkedAt", ""}, {"error", ""}});
      } else {
        out.push_back({{"url", url}, {"healthy", it->second.healthy},
                       {"checked", it->second.checked},
                       {"latencyMs", it->second.latency_ms},
                       {"checkedAt", it->second.checked_at},
                       {"error", it->second.error}});
      }
    }
    return out;
  }

  void check_health() {
    std::vector<std::string> urls;
    if (config.upstreams.empty()) {
      urls.push_back(config.target);
    } else {
      for (const auto& upstream : config.upstreams) urls.push_back(upstream.url);
    }
    for (const auto& url : urls) {
      if (stop_health.load()) return;
      HealthState state;
      state.checked = true;
      state.checked_at = now_iso8601();
      const ParsedUrl parsed = parse_url(url);
      if (!parsed.valid) {
        state.healthy = false;
        state.error = "invalid upstream URL";
      } else {
        std::unique_ptr<uvcpp::uvcpp_ssl_context> ssl;
        const bool tls = parsed.scheme == "https";
        if (tls) {
          ssl.reset(new uvcpp::uvcpp_ssl_context(uvcpp::tls_mode::CLIENT,
                                                 uvcpp::tls_version::TLS_1_2));
          const std::string ca = find_ca_bundle(executable_dir(nullptr));
          if (!ca.empty()) ssl->load_ca_file(ca);
          ssl->set_verify_mode(uvcpp::tls_verify_mode::PEER_STRICT);
        }
        uvcpp::uvcpp_http_client client;
        if (ssl) client.set_ssl_context(ssl.get());
        const auto started = std::chrono::steady_clock::now();
        const int rc = client.connect_wait(parsed.host.c_str(), parsed.port, 3000);
        state.latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - started)
                               .count();
        state.healthy = rc == 0;
        if (rc != 0) state.error = uv_error_string(rc);
      }
      {
        std::lock_guard<std::mutex> lock(health_mutex);
        health[url] = state;
      }
    }
  }

  void health_loop() {
    while (!stop_health.load()) {
      check_health();
      std::unique_lock<std::mutex> lock(health_wait_mutex);
      health_cv.wait_for(lock, std::chrono::seconds(10),
                         [this] { return stop_health.load(); });
    }
  }

  void start_health() {
    if (config.upstreams.size() <= 1) return;
    stop_health.store(false);
    if (health_thread.joinable()) health_thread.join();
    health_thread = std::thread([this] { health_loop(); });
  }

  void stop_health_checks() {
    stop_health.store(true);
    health_cv.notify_all();
    if (health_thread.joinable()) health_thread.join();
  }

  void record(int status, double latency_ms) {
    std::lock_guard<std::mutex> lock(metrics_mutex);
    samples.push_back({std::chrono::steady_clock::now(), status, latency_ms});
    while (samples.size() > 2048) samples.pop_front();
  }
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
    std::vector<ProxyCandidate> candidates;
    if (config.upstreams.empty()) {
      ProxyCandidate candidate;
      candidate.url = config.target;
      candidate.parsed = parse_url(config.target);
      candidate.weight = 1;
      if (candidate.parsed.valid) candidates.push_back(std::move(candidate));
    } else {
      for (const auto& upstream : config.upstreams) {
        ProxyCandidate candidate;
        candidate.url = upstream.url;
        candidate.parsed = parse_url(upstream.url);
        candidate.weight = std::max(1, upstream.weight);
        if (candidate.parsed.valid && (candidate.parsed.scheme == "http" ||
                                       candidate.parsed.scheme == "https")) {
          candidates.push_back(std::move(candidate));
        }
      }
    }
    bool any_healthy = false;
    for (const auto& candidate : candidates) {
      if (is_healthy(candidate.url)) any_healthy = true;
    }
    if (any_healthy) {
      candidates.erase(
          std::remove_if(candidates.begin(), candidates.end(),
                         [this](const ProxyCandidate& candidate) {
                           return !is_healthy(candidate.url);
                         }),
          candidates.end());
    }
    if (candidates.empty()) {
      errors.fetch_add(1);
      resp.status(502).json_str("{\"error\":\"invalid proxy target\"}");
      resp.end();
      add_log({{"method", req.method_name()}, {"path", req.raw_url()}, {"status", 502},
               {"error", "invalid proxy target"}, {"latencyMs", 0}});
      return;
    }

    std::string path = req.raw_path();
    if (path.empty()) path = "/";
    std::string query = req.query_string();
    bool rewritten = false;

    // First enabled matching rule wins. Redirects answer immediately; rewrites
    // replace the upstream path (the target base path then does not apply).
    for (const auto& rule : config.rules) {
      if (!rule.enabled) continue;
      if (!rule.method.empty() && !method_matches(rule.method, req.method_name())) {
        continue;
      }
      if (!rule.host.empty() && !host_matches(rule.host, req.host())) continue;
      if (!rule.header_name.empty()) {
        const std::string value = req.header(rule.header_name);
        if (rule.header_value.empty() ? value.empty() : value != rule.header_value) {
          continue;
        }
      }
      bool matched = false;
      std::string rest;
      std::smatch capture;
      if (rule.pattern.empty()) {
        matched = true;
      } else if (rule.match_type == "exact") {
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
        record(rule.status, static_cast<double>(elapsed));
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

    std::vector<std::pair<std::string, std::string>> forwarded_headers;
    for (const auto& header : req.headers()) {
      if (hop_by_hop(header.name)) continue;
      if (lower(header.name) == "host") continue;
      forwarded_headers.emplace_back(header.name, header.value);
    }
    if (!req.peer_ip().empty()) {
      forwarded_headers.emplace_back("x-forwarded-for", req.peer_ip());
    }
    forwarded_headers.emplace_back("x-forwarded-host", req.host());
    forwarded_headers.emplace_back("x-forwarded-proto", config.tls ? "https" : "http");
    forwarded_headers.emplace_back("x-uvscope-proxy", config.id);

    const std::string request_method = lower(req.method_name());
    const bool idempotent = request_method == "get" || request_method == "head" ||
                            request_method == "options" || request_method == "trace";
    const int max_attempts = 1 + std::min(3, std::max(0, config.retries));
    std::vector<bool> tried(candidates.size(), false);
    uvcpp::uvcpp_http_response response;
    int rc = 0;
    int attempts = 0;
    std::string last_error;
    std::string used_url;

    for (int attempt = 0; attempt < max_attempts; ++attempt) {
      const int index = pick_candidate(rng, candidates, tried);
      if (index < 0) break;
      tried[static_cast<std::size_t>(index)] = true;
      const ProxyCandidate& candidate = candidates[static_cast<std::size_t>(index)];
      const ParsedUrl& target = candidate.parsed;
      used_url = candidate.url;

      std::string attempt_path = path;
      if (!rewritten && target.path != "/" && !target.path.empty()) {
        attempt_path = join_path(target.path, attempt_path);
      }
      if (!query.empty()) attempt_path += "?" + query;

      uvcpp::uvcpp_http_request upstream;
      upstream.method = req.method();
      upstream.url = attempt_path;
      for (const auto& header : forwarded_headers) {
        upstream.set_header(header.first, header.second);
      }
      const int default_port = target.scheme == "https" ? 443 : 80;
      upstream.set_header("host", target.port == default_port
                                      ? target.host
                                      : target.host + ":" + std::to_string(target.port));
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
      ++attempts;
      const int connect_rc = client.connect_wait(target.host.c_str(), target.port, 15000);
      if (connect_rc != 0) {
        rc = connect_rc;
        last_error = uv_error_string(connect_rc);
        // Nothing was sent, so a retry is safe for any method.
        continue;
      }
      uvcpp::uvcpp_http_response attempt_response;
      const int send_rc = ssl ? client.send_wait_ssl(upstream, attempt_response, 30000)
                              : client.send_wait_plain(upstream, attempt_response, 30000);
      if (send_rc != 0) {
        rc = send_rc;
        last_error = uv_error_string(send_rc);
        if (idempotent) continue;
        break;
      }
      response = std::move(attempt_response);
      rc = 0;
      const int status = static_cast<int>(response.status_code);
      if (idempotent && (status == 502 || status == 503 || status == 504) &&
          attempt + 1 < max_attempts) {
        last_error = "upstream status " + std::to_string(status);
        continue;
      }
      break;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    if (rc != 0) {
      errors.fetch_add(1);
      const std::string message = last_error.empty() ? "upstream failed" : last_error;
      resp.status(502).set_header("content-type", "application/json")
          .json_str(nlohmann::json({{"error", "upstream request failed"},
                                    {"detail", message},
                                    {"proxy", config.name},
                                    {"attempts", attempts},
                                    {"upstream", used_url}})
                        .dump());
      resp.end();
      record(502, static_cast<double>(elapsed));
      add_log({{"method", req.method_name()}, {"path", req.raw_url()}, {"status", 502},
               {"error", message}, {"attempts", attempts}, {"upstream", used_url},
               {"latencyMs", elapsed}, {"bytes", 0}});
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
    record(static_cast<int>(response.status_code), static_cast<double>(elapsed));
    add_log({{"method", req.method_name()}, {"path", req.raw_url()},
             {"status", static_cast<int>(response.status_code)}, {"latencyMs", elapsed},
             {"attempts", attempts}, {"upstream", used_url}, {"bytes", body.size()}});
  }

  nlohmann::json metrics_snapshot() const {
    const auto now = std::chrono::steady_clock::now();
    const auto window = std::chrono::seconds(60);
    std::vector<double> latencies;
    std::uint64_t recent = 0;
    std::uint64_t status_2xx = 0;
    std::uint64_t status_3xx = 0;
    std::uint64_t status_4xx = 0;
    std::uint64_t status_5xx = 0;
    {
      std::lock_guard<std::mutex> lock(metrics_mutex);
      for (auto it = samples.rbegin(); it != samples.rend(); ++it) {
        if (now - it->at > window) break;
        ++recent;
        latencies.push_back(it->latency_ms);
        if (it->status >= 200 && it->status <= 299) ++status_2xx;
        else if (it->status >= 300 && it->status <= 399) ++status_3xx;
        else if (it->status >= 400 && it->status <= 499) ++status_4xx;
        else if (it->status >= 500) ++status_5xx;
      }
    }
    auto percentile = [&latencies](double p) -> long long {
      if (latencies.empty()) return 0;
      std::sort(latencies.begin(), latencies.end());
      std::size_t index = static_cast<std::size_t>(p * latencies.size());
      if (index >= latencies.size()) index = latencies.size() - 1;
      return static_cast<long long>(latencies[index]);
    };
    const long long p50 = percentile(0.50);
    const long long p95 = percentile(0.95);
    return {{"windowSeconds", 60},
            {"qps", static_cast<double>(recent) / 60.0},
            {"p50Ms", p50},
            {"p95Ms", p95},
            {"status2xx", status_2xx},
            {"status3xx", status_3xx},
            {"status4xx", status_4xx},
            {"status5xx", status_5xx}};
  }

  ~Runtime() {
    stop_health_checks();
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
  runtime->start_health();
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
  runtime->stop_health_checks();
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
    runtime->stop_health_checks();
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
                   {"bytes", runtime->bytes.load()},
                   {"metrics", runtime->metrics_snapshot()},
                   {"health", runtime->health_json()}});
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
