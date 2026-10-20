#include "application.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <thread>
#include <vector>

#include <uv.h>
#include <nlohmann/json.hpp>
#include <req/uvcpp_work.h>
#include <uvcpp/uvcpp_threadpool.h>
#include <uvcpp/uvcpp_version.h>
#include <ssl/uvcpp_ssl_context.h>
#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>
#include <webapp/uvcpp_web_static.h>

#include "certificate.h"
#include "crypto.h"
#include "http_probe.h"
#include "net_probe.h"
#include "util.h"
#include "version.h"

namespace uvscope {
namespace {

volatile std::sig_atomic_t g_signal_stop = 0;

extern "C" void handle_stop_signal(int) { g_signal_stop = 1;  }

void send_json(uvcpp::uvcpp_web_response& resp, int status, const nlohmann::json& body) {
  std::string text = body.dump();
  resp.status(status).set_header("Cache-Control", "no-store").json_str(std::move(text));
  resp.end();
}

void send_ok(uvcpp::uvcpp_web_response& resp, nlohmann::json body = nlohmann::json::object()) {
  body["ok"] = true;
  send_json(resp, 200, body);
}

void send_error(uvcpp::uvcpp_web_response& resp, int status, const std::string& error,
                const std::string& code = std::string()) {
  nlohmann::json body = {{"ok", false}, {"error", error}};
  if (!code.empty()) body["code"] = code;
  send_json(resp, status, body);
}

std::optional<nlohmann::json> parse_json(const uvcpp::uvcpp_web_request& req,
                                         uvcpp::uvcpp_web_response& resp) {
  if (!req.is_json()) {
    send_error(resp, 415, "expected application/json", "unsupported_media_type");
    return std::nullopt;
  }
  nlohmann::json body = nlohmann::json::parse(req.body_str(), nullptr, false);
  if (body.is_discarded() || !body.is_object()) {
    send_error(resp, 400, "invalid JSON body", "bad_request");
    return std::nullopt;
  }
  return body;
}

std::string string_field(const nlohmann::json& body, const char* key,
                         const std::string& fallback = std::string()) {
  if (!body.contains(key) || !body[key].is_string()) return fallback;
  return body[key].get<std::string>();
}

int int_field(const nlohmann::json& body, const char* key, int fallback) {
  if (!body.contains(key) || !body[key].is_number_integer()) return fallback;
  return body[key].get<int>();
}

bool bool_field(const nlohmann::json& body, const char* key, bool fallback) {
  if (!body.contains(key) || !body[key].is_boolean()) return fallback;
  return body[key].get<bool>();
}

bool safe_preview_base(const std::string& value) {
  if (value.size() > 4096) return false;
  if (value.rfind("http://", 0) != 0 && value.rfind("https://", 0) != 0) return false;
  return value.find_first_of("\"'<>\r\n") == std::string::npos;
}

std::string html_attr_escape(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (char c : value) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '"': out += "&quot;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

std::string preview_document(std::string html, const std::string& base_url) {
  if (base_url.empty()) return html;
  const std::string base = "<base href=\"" + html_attr_escape(base_url) + "\">";
  if (std::regex_search(html, std::regex("<head[^>]*>", std::regex::icase))) {
    return std::regex_replace(html, std::regex("<head[^>]*>", std::regex::icase),
                              "$&" + base, std::regex_constants::format_first_only);
  }
  if (std::regex_search(html, std::regex("<html[^>]*>", std::regex::icase))) {
    return std::regex_replace(html, std::regex("<html[^>]*>", std::regex::icase),
                              "$&<head>" + base + "</head>",
                              std::regex_constants::format_first_only);
  }
  return "<head>" + base + "</head>" + html;
}

Socks5Proxy proxy_field(const nlohmann::json& body) {
  Socks5Proxy proxy;
  if (!body.contains("proxy") || !body["proxy"].is_object()) return proxy;
  const auto& value = body["proxy"];
  proxy.host = string_field(value, "host");
  proxy.port = int_field(value, "port", 0);
  proxy.username = string_field(value, "username");
  proxy.password = string_field(value, "password");
  return proxy;
}

std::vector<ProxyRule> proxy_rules_field(const nlohmann::json& body, std::string* error) {
  std::vector<ProxyRule> rules;
  if (!body.contains("rules")) return rules;
  if (!body["rules"].is_array()) {
    if (error) *error = "rules must be an array";
    return {};
  }
  if (body["rules"].size() > 64) {
    if (error) *error = "at most 64 routing rules are allowed";
    return {};
  }
  std::size_t index = 0;
  for (const auto& item : body["rules"]) {
    ++index;
    const std::string label = "rule " + std::to_string(index);
    if (!item.is_object()) {
      if (error) *error = label + " must be an object";
      return {};
    }
    ProxyRule rule;
    rule.id = string_field(item, "id");
    if (rule.id.empty()) rule.id = make_id();
    rule.name = string_field(item, "name");
    rule.enabled = bool_field(item, "enabled", true);
    rule.match_type = lower(string_field(item, "matchType", "prefix"));
    rule.pattern = string_field(item, "pattern");
    rule.action = lower(string_field(item, "action", "redirect"));
    rule.status = int_field(item, "status", 302);
    rule.destination = string_field(item, "destination");
    rule.preserve_query = bool_field(item, "preserveQuery", true);
    if (rule.match_type != "exact" && rule.match_type != "prefix" &&
        rule.match_type != "regex") {
      if (error) *error = label + ": matchType must be exact, prefix or regex";
      return {};
    }
    if (rule.pattern.empty() || rule.pattern.size() > 512) {
      if (error) *error = label + ": pattern must be 1-512 characters";
      return {};
    }
    if (rule.match_type == "regex") {
      try {
        std::regex compiled(rule.pattern);
        (void)compiled;
      } catch (const std::regex_error& err) {
        if (error) *error = label + ": invalid regex (" + err.what() + ")";
        return {};
      }
    }
    if (rule.action != "redirect" && rule.action != "rewrite") {
      if (error) *error = label + ": action must be redirect or rewrite";
      return {};
    }
    if (rule.status != 301 && rule.status != 302 && rule.status != 303 &&
        rule.status != 307 && rule.status != 308) {
      if (error) *error = label + ": status must be 301, 302, 303, 307 or 308";
      return {};
    }
    if (rule.destination.empty() || rule.destination.size() > 2048) {
      if (error) *error = label + ": destination must be 1-2048 characters";
      return {};
    }
    if (rule.action == "rewrite" && rule.destination.front() != '/' &&
        rule.destination.front() != '$') {
      if (error) *error = label + ": rewrite destinations must start with / or a variable";
      return {};
    }
    rules.push_back(std::move(rule));
  }
  return rules;
}

std::vector<HttpHeaderInput> header_fields(const nlohmann::json& body) {
  std::vector<HttpHeaderInput> headers;
  if (!body.contains("headers") || !body["headers"].is_array()) return headers;
  for (const auto& item : body["headers"]) {
    if (!item.is_object()) continue;
    const std::string name = string_field(item, "name");
    if (name.empty()) continue;
    headers.push_back({name, string_field(item, "value")});
  }
  return headers;
}

std::vector<int> version_parts(std::string text) {
  text = trim(text);
  if (!text.empty() && (text[0] == 'v' || text[0] == 'V')) text.erase(text.begin());
  std::vector<int> parts;
  std::string current;
  for (char c : text) {
    if (c >= '0' && c <= '9') {
      current.push_back(c);
    } else if (!current.empty()) {
      parts.push_back(std::atoi(current.c_str()));
      current.clear();
      if (c == '-') break;
    }
  }
  if (!current.empty()) parts.push_back(std::atoi(current.c_str()));
  while (parts.size() < 3) parts.push_back(0);
  return parts;
}

bool version_newer(const std::string& latest, const std::string& current) {
  const auto a = version_parts(latest);
  const auto b = version_parts(current);
  return a > b;
}

std::string host_for_url(const std::string& configured) {
  if (configured.empty() || configured == "0.0.0.0" || configured == "::" || configured == "[::]") {
    return "127.0.0.1";
  }
  if (configured.find(':') != std::string::npos && configured.front() != '[') {
    return "[" + configured + "]";
  }
  return configured;
}

void open_browser(const std::string& url) {
#if defined(_WIN32)
  std::string command = "start \"\" \"" + url + "\"";
#elif defined(__APPLE__)
  std::string command = "open \"" + url + "\" >/dev/null 2>&1 &";
#else
  std::string command = "xdg-open \"" + url + "\" >/dev/null 2>&1 &";
#endif
  const int ignored = std::system(command.c_str());
  (void)ignored;
}

std::string platform_name() {
#if defined(_WIN32)
  return "Windows";
#elif defined(__APPLE__)
  return "macOS";
#elif defined(__linux__)
  return "Linux";
#else
  return "Unknown";
#endif
}

bool module_id_valid(const std::string& module) {
  return module == "http" || module == "websocket" || module == "tcpudp" || module == "soap" ||
         module == "tls" || module == "proxy" || module == "all";
}

}  // namespace

Application::Application(AppOptions options)
    : options_(std::move(options)),
      store_(),
      auth_(store_),
      proxies_(nullptr),
      app_(),
      started_(std::chrono::steady_clock::now()),
      started_at_(now_iso8601()) {}

Application::~Application() {
  if (update_thread_.joinable()) update_thread_.join();

  socks5_.reset();
  transport_.reset();

  if (proxies_) proxies_->stop_all();
}

bool Application::initialize() {
  std::string error;
  if (!ensure_directory(options_.data_dir, &error)) {
    std::fprintf(stderr, "uvscope: cannot create data directory: %s\n", error.c_str());
    return false;
  }
  if (!store_.open(options_.data_dir / "uvscope.db", &error)) {
    std::fprintf(stderr, "uvscope: cannot open database: %s\n", error.c_str());
    return false;
  }
  proxies_.reset(new ProxyManager(store_));
  transport_.reset(new TransportManager());
  if (!transport_->available()) {
    std::fprintf(stderr, "uvscope: transport sessions unavailable: %s\n",
                 transport_->start_error().c_str());
  } else {
    for (const auto& listener : store_.listeners()) {
      if (!transport_->add_listener(listener, nullptr)) continue;
      if (listener.auto_start) transport_->start_listener(listener.id, nullptr);
    }
  }
  socks5_.reset(new Socks5Manager());
  for (const auto& server : store_.socks5_servers()) {
    if (!socks5_->add_server(server, nullptr)) continue;
    if (server.auto_start) socks5_->start_server(server.id, nullptr);
  }
  web_root_ = options_.web_root.string();
  if (!web_root_.empty()) {
    std::error_code ec;
    web_root_ = std::filesystem::weakly_canonical(options_.web_root, ec).string();
  }

  app_.set_host(options_.host)
      .set_port(options_.port)
      .set_max_body_size(16 * 1024 * 1024)
      .set_access_log(false)
      .set_server_header("uvscope")
      .set_compression(true)
      .set_shutdown_grace_ms(1500);
  if (!options_.tls_cert.empty() || !options_.tls_key.empty()) {
    if (options_.tls_cert.empty() || options_.tls_key.empty()) {
      std::fprintf(stderr, "uvscope: --tls-cert and --tls-key must be supplied together\n");
      return false;
    }
    app_.enable_ssl(options_.tls_cert, options_.tls_key);
    if (!app_.ssl_error().empty()) {
      std::fprintf(stderr, "uvscope: TLS setup failed: %s\n", app_.ssl_error().c_str());
      return false;
    }
  }

  register_middleware();
  register_public_routes();
  register_auth_routes();
  register_dashboard_routes();
  register_debug_routes();
  register_socks5_routes();
  register_proxy_routes();
  register_admin_routes();
  register_update_routes();
  install_static();
  return true;
}

int Application::run() {
  uvcpp::uvcpp_set_threadpool_size(0);
  if (options_.self_check) return self_check();
  if (!initialize()) return 1;

  const int rc = app_.start();
  if (rc != 0) {
    std::fprintf(stderr, "uvscope: failed to listen on %s:%d: %s\n", options_.host.c_str(),
                 options_.port, uv_error_string(rc).c_str());
    return 1;
  }

  g_signal_stop = 0;
  std::signal(SIGINT, handle_stop_signal);
  std::signal(SIGTERM, handle_stop_signal);
  std::thread watcher([] {
    while (g_signal_stop == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
  });
  watcher.detach();

  const bool tls = !options_.tls_cert.empty();
  const std::string url = std::string(tls ? "https://" : "http://") + host_for_url(options_.host) +
                          ":" + std::to_string(app_.bound_port());
  std::printf("uvscope %s\n", UVSCOPE_VERSION);
  std::printf("  workbench : %s\n", url.c_str());
  std::printf("  data      : %s\n", store_.database_path().string().c_str());
  std::printf("  web root  : %s\n", web_root_.c_str());
  std::printf("  libuvcpp  : %s\n", uvcpp::version_string());
  std::printf("  engine    : libuv %s\n", libuv_version_string().c_str());
  std::fflush(stdout);
  if (options_.open_browser) open_browser(url);

  // The app runs its loop on a background thread. Wake periodically to notice
  // a signal and to run the optional GitHub release check without blocking the
  // control plane.
  auto next_update = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (app_.running() && g_signal_stop == 0) {
    if (std::chrono::steady_clock::now() >= next_update) {
      const bool auto_update = store_.setting("update.auto", "1") == "1";
      if (auto_update) {
        if (update_thread_.joinable()) update_thread_.join();
        update_thread_ = std::thread([this] {
          try {
            update_payload();
          } catch (...) {
            // A release check is best-effort; never terminate the daemon.
          }
        });
      }
      const int hours = std::max(1, std::atoi(store_.setting("update.interval", "24").c_str()));
      next_update = std::chrono::steady_clock::now() + std::chrono::hours(hours);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  app_.stop();

  app_.join();

  if (update_thread_.joinable()) update_thread_.join();
  if (proxies_) proxies_->stop_all();
  store_.close();
  return 0;
}

// ---------------------------------------------------------------------------
// Middleware
// ---------------------------------------------------------------------------
void Application::register_middleware() {
  app_.use([](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& resp,
              uvcpp::uvcpp_web_next next) {
    resp.set_header("X-Content-Type-Options", "nosniff");
    resp.set_header("X-Frame-Options", "DENY");
    resp.set_header("Referrer-Policy", "no-referrer");
    resp.set_header("Permissions-Policy", "camera=(), microphone=(), geolocation=()");
    resp.set_header("Content-Security-Policy",
                    "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
                    "img-src 'self' data: blob: http: https:; "
                    "media-src 'self' data: blob: http: https:; "
                    "font-src 'self' data:; connect-src 'self' ws: wss: http: https:; "
                    "frame-src 'self' blob:; frame-ancestors 'none'; base-uri 'self'; "
                    "form-action 'self'");
    next();
  });
}

void Application::install_static() {
  uvcpp::uvcpp_web_static_options opts;
  opts.spa_fallback = true;
  opts.spa_file = "index.html";
  opts.range = true;
  opts.etag = true;
  opts.last_modified = true;
  opts.cache_control = "no-cache";
  opts.max_file_size = 64 * 1024 * 1024;
  if (std::filesystem::exists(std::filesystem::path(web_root_) / "index.html")) {
    app_.serve_static("/", web_root_, opts);
  } else {
    app_.get("/", [this](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& resp,
                         uvcpp::uvcpp_web_next) {
      resp.status(503).html(
          "<!doctype html><meta charset=utf-8><title>uvscope</title>"
          "<style>body{font:16px/1.6 system-ui;background:#0d1117;color:#e6edf3;padding:3rem}"
          "code{background:#161b22;padding:.2em .4em;border-radius:4px}</style>"
          "<h1>uvscope</h1><p>The frontend bundle was not found.</p>"
          "<p>Build it with <code>npm --prefix web ci && npm --prefix web run build</code>, "
          "or pass <code>--web-root</code>.</p><p>API health: <code>/api/health</code></p>");
      resp.end();
    });
  }
  app_.any("/api/*path", [](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                            uvcpp::uvcpp_web_next) {
    send_error(resp, 404, "API route not found: " + req.path(), "not_found");
  });
  app_.any("/api", [](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& resp,
                      uvcpp::uvcpp_web_next) {
    send_error(resp, 404, "API route not found", "not_found");
  });
}

// ---------------------------------------------------------------------------
// Public routes
// ---------------------------------------------------------------------------
void Application::register_public_routes() {
  app_.get("/api/health", [this](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& resp,
                                 uvcpp::uvcpp_web_next) {
    send_ok(resp, {{"name", "uvscope"},
                   {"version", UVSCOPE_VERSION},
                   {"libuvcpp", uvcpp::version_string()},
                   {"uptimeSeconds",
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - started_)
                        .count()},
                   {"setupRequired", !store_.initialized()}});
  });

  app_.get("/api/setup/status", [this](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& resp,
                                       uvcpp::uvcpp_web_next) {
    const bool initialized = store_.initialized();
    send_json(resp, 200,
              {{"ok", true},
               {"initialized", initialized},
               {"setupRequired", !initialized},
               {"version", UVSCOPE_VERSION},
               {"libuvcpp", uvcpp::version_string()},
               {"dataDir", store_.database_path().parent_path().string()}});
  });

  app_.post("/api/setup/init", [this](uvcpp::uvcpp_web_request& req,
                                      uvcpp::uvcpp_web_response& resp,
                                      uvcpp::uvcpp_web_next) {
    if (store_.initialized()) {
      send_error(resp, 409, "uvscope is already initialized", "already_initialized");
      return;
    }
    const auto body = parse_json(req, resp);
    if (!body) return;
    const std::string username = string_field(*body, "username");
    const std::string password = string_field(*body, "password");
    if (username.empty() || password.empty()) {
      send_error(resp, 400, "username and password are required", "bad_request");
      return;
    }
    std::string error;
    if (!store_.initialize_admin(username, password, &error)) {
      send_error(resp, 400, error, "setup_failed");
      return;
    }
    const auto user = store_.user_by_username(username);
    if (!user) {
      send_error(resp, 500, "administrator created but could not be reloaded");
      return;
    }
    const std::string token = store_.create_session(user->id);
    Auth::set_cookie(resp, token, 7 * 24 * 60 * 60, !options_.tls_cert.empty());
    store_.set_last_login(user->id);
    send_json(resp, 200, {{"ok", true}, {"user", user_json(*user)},
                          {"modules", store_.module_states()}});
  });

  app_.post("/api/auth/login", [this](uvcpp::uvcpp_web_request& req,
                                      uvcpp::uvcpp_web_response& resp,
                                      uvcpp::uvcpp_web_next) {
    const auto body = parse_json(req, resp);
    if (!body) return;
    const std::string username = string_field(*body, "username");
    const std::string password = string_field(*body, "password");
    if (username.empty() || password.empty()) {
      send_error(resp, 400, "username and password are required", "bad_request");
      return;
    }
    std::string error;
    const auto user = store_.verify_login(username, password, &error);
    if (!user) {
      send_error(resp, 401, "invalid username or password", "invalid_credentials");
      return;
    }
    const std::string token = store_.create_session(user->id);
    Auth::set_cookie(resp, token, 7 * 24 * 60 * 60, !options_.tls_cert.empty());
    store_.set_last_login(user->id);
    send_json(resp, 200, {{"ok", true}, {"user", user_json(*user)},
                          {"modules", store_.module_states()}});
  });
}

// ---------------------------------------------------------------------------
// Authenticated routes
// ---------------------------------------------------------------------------
void Application::register_auth_routes() {
  app_.post("/api/auth/logout", [this](uvcpp::uvcpp_web_request& req,
                                       uvcpp::uvcpp_web_response& resp,
                                       uvcpp::uvcpp_web_next) {
    const std::string token = session_token(req);
    if (!token.empty()) store_.delete_session(token);
    Auth::clear_cookie(resp);
    send_ok(resp);
  });

  app_.get("/api/auth/me", [this](uvcpp::uvcpp_web_request& req,
                                  uvcpp::uvcpp_web_response& resp,
                                  uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require(req, resp, "dashboard");
    if (!principal) return;
    send_json(resp, 200,
              {{"ok", true},
               {"user", user_json(principal->user)},
               {"modules", store_.module_states()},
               {"permissions",
                {{"admin.users", role_can(principal->user.role, "admin.users")},
                 {"admin.settings", role_can(principal->user.role, "admin.settings")},
                 {"admin.reset", role_can(principal->user.role, "admin.reset")},
                 {"debug", role_can(principal->user.role, "debug")}}}});
  });

  app_.post("/api/auth/password", [this](uvcpp::uvcpp_web_request& req,
                                         uvcpp::uvcpp_web_response& resp,
                                         uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require(req, resp, "dashboard");
    if (!principal) return;
    const auto body = parse_json(req, resp);
    if (!body) return;
    const std::string old_password = string_field(*body, "currentPassword");
    const std::string new_password = string_field(*body, "newPassword");
    std::string error;
    if (!store_.verify_login(principal->user.username, old_password, &error)) {
      send_error(resp, 400, "current password is incorrect", "invalid_credentials");
      return;
    }
    if (!store_.change_password(principal->user.id, new_password, &error)) {
      send_error(resp, 400, error, "password_change_failed");
      return;
    }
    store_.delete_sessions_for_user(principal->user.id);
    const std::string token = store_.create_session(principal->user.id);
    Auth::set_cookie(resp, token, 7 * 24 * 60 * 60, !options_.tls_cert.empty());
    send_ok(resp);
  });
}

nlohmann::json Application::user_json(const User& user) const {
  return {{"id", user.id},
          {"username", user.username},
          {"role", user.role},
          {"roleDescription", role_permission_summary(user.role)},
          {"enabled", user.enabled},
          {"createdAt", user.created_at},
          {"lastLogin", user.last_login}};
}

// ---------------------------------------------------------------------------
// Dashboard / history
// ---------------------------------------------------------------------------
void Application::register_dashboard_routes() {
  app_.get("/api/dashboard", [this](uvcpp::uvcpp_web_request& req,
                                    uvcpp::uvcpp_web_response& resp,
                                    uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require(req, resp, "dashboard");
    if (!principal) return;
    const auto recent = store_.history("", 24);
    nlohmann::json activity = nlohmann::json::array();
    std::map<std::string, std::int64_t> counts;
    for (const auto& item : recent) {
      counts[item.kind] += 1;
      activity.push_back({{"id", item.id},
                          {"kind", item.kind},
                          {"summary", item.summary},
                          {"status", item.status},
                          {"httpStatus", item.http_status},
                          {"latencyMs", item.latency_ms},
                          {"sizeBytes", item.size_bytes},
                          {"createdAt", item.created_at}});
    }
    const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - started_)
                            .count();
    send_json(resp, 200,
              {{"ok", true},
               {"server",
                {{"version", UVSCOPE_VERSION},
                 {"libuvcpp", uvcpp::version_string()},
                 {"libuv", libuv_version_string()},
                 {"platform", platform_name()},
                 {"startedAt", started_at_},
                 {"uptimeSeconds", uptime},
                 {"host", options_.host},
                 {"port", options_.port},
                 {"tls", !options_.tls_cert.empty()},
                 {"dataDir", store_.database_path().parent_path().string()},
                 {"webRoot", web_root_},
                 {"rssKb", resident_memory_kb()}}},
               {"metrics",
                {{"httpRequests", metrics_.http_requests.load()},
                 {"httpErrors", metrics_.http_errors.load()},
                 {"tcpProbes", metrics_.tcp_probes.load()},
                 {"udpProbes", metrics_.udp_probes.load()},
                 {"tlsInspections", metrics_.tls_inspections.load()},
                 {"proxyRequests", proxies_ ? proxies_->total_requests() : 0},
                 {"proxyErrors", proxies_ ? proxies_->total_errors() : 0},
                 {"activeProxies", proxies_ ? proxies_->active_count() : 0},
                 {"users", store_.user_count()},
                 {"historyEntries", recent.size()}}},
               {"activityCounts", counts},
               {"modules", store_.module_states()},
               {"proxies", proxy_view()},
               {"recent", activity}});
  });

  app_.get("/api/history", [this](uvcpp::uvcpp_web_request& req,
                                  uvcpp::uvcpp_web_response& resp,
                                  uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require(req, resp, "history.read");
    if (!principal) return;
    const std::string* kind = req.query("kind");
    const std::string* limit_text = req.query("limit");
    int limit = 100;
    if (limit_text != nullptr) {
      const int parsed = std::atoi(limit_text->c_str());
      if (parsed > 0) limit = parsed;
    }
    nlohmann::json entries = nlohmann::json::array();
    for (const auto& item : store_.history(kind ? *kind : std::string(), limit)) {
      nlohmann::json detail = nlohmann::json::parse(item.detail, nullptr, false);
      if (detail.is_discarded()) detail = nlohmann::json::object();
      entries.push_back({{"id", item.id},
                         {"kind", item.kind},
                         {"summary", item.summary},
                         {"status", item.status},
                         {"httpStatus", item.http_status},
                         {"latencyMs", item.latency_ms},
                         {"sizeBytes", item.size_bytes},
                         {"detail", detail},
                         {"createdAt", item.created_at}});
    }
    send_json(resp, 200, {{"ok", true}, {"entries", entries}});
  });

  app_.del("/api/history", [this](uvcpp::uvcpp_web_request& req,
                                  uvcpp::uvcpp_web_response& resp,
                                  uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require(req, resp, "admin.reset");
    if (!principal) return;
    const std::string* kind = req.query("kind");
    std::string error;
    if (!store_.clear_history(kind ? *kind : std::string(), &error)) {
      send_error(resp, 500, error);
      return;
    }
    send_ok(resp);
  });
}

// ---------------------------------------------------------------------------
// Debug modules
// ---------------------------------------------------------------------------
void Application::register_debug_routes() {
  app_.post("/api/http/send", [this](uvcpp::uvcpp_web_request& req,
                                     uvcpp::uvcpp_web_response& resp,
                                     uvcpp::uvcpp_web_next next) {
    const auto principal = auth_.require_module(req, resp, "http", "debug");
    if (!principal) return;
    const auto body = parse_json(req, resp);
    if (!body) return;
    HttpProbeInput input;
    input.url = string_field(*body, "url");
    input.method = string_field(*body, "method", "GET");
    input.headers = header_fields(*body);
    input.body = string_field(*body, "body");
    input.timeout_ms = int_field(*body, "timeoutMs", 20000);
    input.protocol = string_field(*body, "protocol", "auto");
    input.insecure = bool_field(*body, "insecure", false);
    input.compression = bool_field(*body, "compression", false);
    input.proxy = proxy_field(*body);
    input.max_body_bytes = 4 * 1024 * 1024;
    if (input.url.empty()) {
      send_error(resp, 400, "url is required", "bad_request");
      return;
    }
    post_job(
        [input] { return JobResult{200, run_http_probe(input)}; }, &resp, next,
        [this, user = principal->user](JobResult& result) {
          const bool ok = result.body.value("ok", false);
          if (ok) {
            metrics_.http_requests.fetch_add(1);
          } else {
            metrics_.http_errors.fetch_add(1);
          }
          const std::string summary =
              result.body.value("request", nlohmann::json::object()).value("method", std::string("HTTP")) +
              " " + result.body.value("request", nlohmann::json::object()).value("url", std::string());
          Principal owner;
          owner.user = user;
          add_history(owner, "http", summary, result.body);
        });
  });

  app_.post("/api/tcp/probe", [this](uvcpp::uvcpp_web_request& req,
                                     uvcpp::uvcpp_web_response& resp,
                                     uvcpp::uvcpp_web_next next) {
    const auto principal = auth_.require_module(req, resp, "tcpudp", "debug");
    if (!principal) return;
    const auto body = parse_json(req, resp);
    if (!body) return;
    TcpProbeInput input;
    input.host = string_field(*body, "host");
    input.port = int_field(*body, "port", 0);
    input.payload = string_field(*body, "payload");
    input.encoding = string_field(*body, "encoding", "text");
    input.connect_timeout_ms = int_field(*body, "connectTimeoutMs", 8000);
    input.read_timeout_ms = int_field(*body, "readTimeoutMs", 3000);
    input.proxy = proxy_field(*body);
    if (input.host.empty() || input.port <= 0) {
      send_error(resp, 400, "host and port are required", "bad_request");
      return;
    }
    post_job(
        [input] { return JobResult{200, run_tcp_probe(input)}; }, &resp, next,
        [this, principal](JobResult& result) {
          metrics_.tcp_probes.fetch_add(1);
          add_history(*principal, "tcp",
                      "TCP " + result.body.value("host", std::string()) + ":" +
                          std::to_string(result.body.value("port", 0)),
                      result.body);
        });
  });

  app_.post("/api/udp/probe", [this](uvcpp::uvcpp_web_request& req,
                                     uvcpp::uvcpp_web_response& resp,
                                     uvcpp::uvcpp_web_next next) {
    const auto principal = auth_.require_module(req, resp, "tcpudp", "debug");
    if (!principal) return;
    const auto body = parse_json(req, resp);
    if (!body) return;
    UdpProbeInput input;
    input.host = string_field(*body, "host");
    input.port = int_field(*body, "port", 0);
    input.payload = string_field(*body, "payload");
    input.encoding = string_field(*body, "encoding", "text");
    input.timeout_ms = int_field(*body, "timeoutMs", 3000);
    input.proxy = proxy_field(*body);
    if (input.host.empty() || input.port <= 0) {
      send_error(resp, 400, "host and port are required", "bad_request");
      return;
    }
    post_job(
        [input] { return JobResult{200, run_udp_probe(input)}; }, &resp, next,
        [this, principal](JobResult& result) {
          metrics_.udp_probes.fetch_add(1);
          add_history(*principal, "udp",
                      "UDP " + result.body.value("host", std::string()) + ":" +
                          std::to_string(result.body.value("port", 0)),
                      result.body);
        });
  });

  app_.post("/api/tcp/sessions", [this](uvcpp::uvcpp_web_request& req,
                                        uvcpp::uvcpp_web_response& resp,
                                        uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require_module(req, resp, "tcpudp", "debug");
    if (!principal) return;
    if (!transport_ || !transport_->available()) {
      send_error(resp, 503, "transport sessions are unavailable", "unavailable");
      return;
    }
    const auto body = parse_json(req, resp);
    if (!body) return;
    const std::string host = string_field(*body, "host");
    const int port = int_field(*body, "port", 0);
    const int timeout_ms = int_field(*body, "connectTimeoutMs", 15000);
    if (host.empty() || port <= 0 || port > 65535) {
      send_error(resp, 400, "host and port are required", "bad_request");
      return;
    }
    std::string error;
    const auto result = transport_->create_tcp_session(host, port, timeout_ms,
                                                       proxy_field(*body), &error);
    if (!result.value("ok", false)) {
      send_error(resp, 400, error.empty() ? "could not create session" : error,
                 "session_failed");
      return;
    }
    send_json(resp, 200, result);
  });

  app_.get("/api/tcp/sessions", [this](uvcpp::uvcpp_web_request& req,
                                       uvcpp::uvcpp_web_response& resp,
                                       uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require_module(req, resp, "tcpudp", "debug");
    if (!principal) return;
    if (!transport_ || !transport_->available()) {
      send_error(resp, 503, "transport sessions are unavailable", "unavailable");
      return;
    }
    send_json(resp, 200, transport_->tcp_sessions());
  });

  app_.get("/api/tcp/sessions/:id", [this](uvcpp::uvcpp_web_request& req,
                                           uvcpp::uvcpp_web_response& resp,
                                           uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require_module(req, resp, "tcpudp", "debug");
    if (!principal) return;
    const std::string* id = req.param("id");
    if (!id || !transport_ || !transport_->available()) {
      send_error(resp, 404, "session not found", "not_found");
      return;
    }
    const auto result = transport_->tcp_session(*id);
    if (!result.value("ok", false)) {
      send_error(resp, 404, result.value("error", std::string("session not found")),
                 "not_found");
      return;
    }
    send_json(resp, 200, result);
  });

  app_.get("/api/tcp/sessions/:id/events", [this](uvcpp::uvcpp_web_request& req,
                                                  uvcpp::uvcpp_web_response& resp,
                                                  uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require_module(req, resp, "tcpudp", "debug");
    if (!principal) return;
    const std::string* id = req.param("id");
    if (!id || !transport_ || !transport_->available()) {
      send_error(resp, 404, "session not found", "not_found");
      return;
    }
    std::uint64_t after = 0;
    if (const std::string* value = req.query("after")) {
      after = std::strtoull(value->c_str(), nullptr, 10);
    }
    int limit = 200;
    if (const std::string* value = req.query("limit")) {
      limit = std::atoi(value->c_str());
    }
    const auto result = transport_->tcp_session_events(*id, after, limit);
    if (!result.value("ok", false)) {
      send_error(resp, 404, result.value("error", std::string("session not found")),
                 "not_found");
      return;
    }
    send_json(resp, 200, result);
  });

  app_.post("/api/tcp/sessions/:id/send", [this](uvcpp::uvcpp_web_request& req,
                                                 uvcpp::uvcpp_web_response& resp,
                                                 uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require_module(req, resp, "tcpudp", "debug");
    if (!principal) return;
    const std::string* id = req.param("id");
    if (!id || !transport_ || !transport_->available()) {
      send_error(resp, 404, "session not found", "not_found");
      return;
    }
    const auto body = parse_json(req, resp);
    if (!body) return;
    std::string error;
    if (!transport_->tcp_send(*id, string_field(*body, "payload"),
                              string_field(*body, "encoding", "text"), &error)) {
      send_error(resp, 400, error, "send_failed");
      return;
    }
    send_json(resp, 200, {{"ok", true}});
  });

  app_.del("/api/tcp/sessions/:id", [this](uvcpp::uvcpp_web_request& req,
                                           uvcpp::uvcpp_web_response& resp,
                                           uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require_module(req, resp, "tcpudp", "debug");
    if (!principal) return;
    const std::string* id = req.param("id");
    if (!id || !transport_ || !transport_->available()) {
      send_error(resp, 404, "session not found", "not_found");
      return;
    }
    std::string error;
    if (!transport_->tcp_close(*id, &error)) {
      send_error(resp, error == "session not found" ? 404 : 400, error,
                 error == "session not found" ? "not_found" : "close_failed");
      return;
    }
    send_json(resp, 200, {{"ok", true}});
  });

  app_.get("/api/transport/listeners", [this](uvcpp::uvcpp_web_request& req,
                                              uvcpp::uvcpp_web_response& resp,
                                              uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require_module(req, resp, "tcpudp", "debug");
    if (!principal) return;
    if (!transport_ || !transport_->available()) {
      send_error(resp, 503, "transport listeners are unavailable", "unavailable");
      return;
    }
    send_json(resp, 200, transport_->listeners());
  });

  app_.post("/api/transport/listeners", [this](uvcpp::uvcpp_web_request& req,
                                               uvcpp::uvcpp_web_response& resp,
                                               uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require_module(req, resp, "tcpudp", "debug");
    if (!principal) return;
    if (!transport_ || !transport_->available()) {
      send_error(resp, 503, "transport listeners are unavailable", "unavailable");
      return;
    }
    const auto body = parse_json(req, resp);
    if (!body) return;
    ListenerConfig config;
    config.id = make_id();
    config.transport = lower(string_field(*body, "transport", "tcp"));
    config.name = string_field(*body, "name");
    config.host = string_field(*body, "host", "127.0.0.1");
    config.port = int_field(*body, "port", 0);
    config.auto_start = bool_field(*body, "autoStart", false);
    config.created_at = now_iso8601();
    if ((config.transport != "tcp" && config.transport != "udp") ||
        config.host.empty() || config.port <= 0 || config.port > 65535) {
      send_error(resp, 400, "transport, host and port are required", "bad_request");
      return;
    }
    std::string error;
    if (!transport_->add_listener(config, &error)) {
      send_error(resp, 400, error, "listener_failed");
      return;
    }
    if (!store_.save_listener(config, &error)) {
      transport_->remove_listener(config.id, nullptr);
      send_error(resp, 500, error, "persist_failed");
      return;
    }
    if (bool_field(*body, "start", true)) {
      if (!transport_->start_listener(config.id, &error)) {
        transport_->remove_listener(config.id, nullptr);
        store_.delete_listener(config.id, nullptr);
        send_error(resp, 400, error, "listener_failed");
        return;
      }
    }
    send_json(resp, 200, {{"ok", true}, {"listener", config.id}});
  });

  app_.post("/api/transport/listeners/:id/start", [this](uvcpp::uvcpp_web_request& req,
                                                         uvcpp::uvcpp_web_response& resp,
                                                         uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require_module(req, resp, "tcpudp", "debug");
    if (!principal) return;
    const std::string* id = req.param("id");
    std::string error;
    if (!id || !transport_ || !transport_->start_listener(*id, &error)) {
      send_error(resp, 400, error.empty() ? "listener not found" : error, "listener_failed");
      return;
    }
    send_json(resp, 200, {{"ok", true}});
  });

  app_.post("/api/transport/listeners/:id/stop", [this](uvcpp::uvcpp_web_request& req,
                                                        uvcpp::uvcpp_web_response& resp,
                                                        uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require_module(req, resp, "tcpudp", "debug");
    if (!principal) return;
    const std::string* id = req.param("id");
    std::string error;
    if (!id || !transport_ || !transport_->stop_listener(*id, &error)) {
      send_error(resp, 400, error.empty() ? "listener not found" : error, "listener_failed");
      return;
    }
    send_json(resp, 200, {{"ok", true}});
  });

  app_.del("/api/transport/listeners/:id", [this](uvcpp::uvcpp_web_request& req,
                                                  uvcpp::uvcpp_web_response& resp,
                                                  uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require_module(req, resp, "tcpudp", "debug");
    if (!principal) return;
    const std::string* id = req.param("id");
    if (!id || !transport_ || !transport_->remove_listener(*id, nullptr)) {
      send_error(resp, 404, "listener not found", "not_found");
      return;
    }
    store_.delete_listener(*id, nullptr);
    send_json(resp, 200, {{"ok", true}});
  });

  app_.get("/api/transport/listeners/:id", [this](uvcpp::uvcpp_web_request& req,
                                                 uvcpp::uvcpp_web_response& resp,
                                                 uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require_module(req, resp, "tcpudp", "debug");
    if (!principal) return;
    const std::string* id = req.param("id");
    if (!id || !transport_) {
      send_error(resp, 404, "listener not found", "not_found");
      return;
    }
    const auto result = transport_->listener(*id);
    if (!result.value("ok", false)) {
      send_error(resp, 404, result.value("error", std::string("listener not found")), "not_found");
      return;
    }
    send_json(resp, 200, result);
  });

  app_.get("/api/transport/listeners/:id/events", [this](uvcpp::uvcpp_web_request& req,
                                                         uvcpp::uvcpp_web_response& resp,
                                                         uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require_module(req, resp, "tcpudp", "debug");
    if (!principal) return;
    const std::string* id = req.param("id");
    if (!id || !transport_) {
      send_error(resp, 404, "listener not found", "not_found");
      return;
    }
    std::uint64_t after = 0;
    if (const std::string* value = req.query("after")) after = std::strtoull(value->c_str(), nullptr, 10);
    int limit = 200;
    if (const std::string* value = req.query("limit")) limit = std::atoi(value->c_str());
    const auto result = transport_->listener_events(*id, after, limit);
    if (!result.value("ok", false)) {
      send_error(resp, 404, result.value("error", std::string("listener not found")), "not_found");
      return;
    }
    send_json(resp, 200, result);
  });

  app_.get("/api/transport/listeners/:id/sessions", [this](uvcpp::uvcpp_web_request& req,
                                                           uvcpp::uvcpp_web_response& resp,
                                                           uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require_module(req, resp, "tcpudp", "debug");
    if (!principal) return;
    const std::string* id = req.param("id");
    if (!id || !transport_) {
      send_error(resp, 404, "listener not found", "not_found");
      return;
    }
    const auto result = transport_->listener_sessions(*id);
    if (!result.value("ok", false)) {
      send_error(resp, 404, result.value("error", std::string("listener not found")), "not_found");
      return;
    }
    send_json(resp, 200, result);
  });

  app_.get("/api/transport/listeners/:id/sessions/:sid/events",
           [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                  uvcpp::uvcpp_web_next) {
             const auto principal = auth_.require_module(req, resp, "tcpudp", "debug");
             if (!principal) return;
             const std::string* id = req.param("id");
             const std::string* sid = req.param("sid");
             if (!id || !sid || !transport_) {
               send_error(resp, 404, "session not found", "not_found");
               return;
             }
             std::uint64_t after = 0;
             if (const std::string* value = req.query("after")) after = std::strtoull(value->c_str(), nullptr, 10);
             int limit = 200;
             if (const std::string* value = req.query("limit")) limit = std::atoi(value->c_str());
             const auto result = transport_->listener_session_events(*id, *sid, after, limit);
             if (!result.value("ok", false)) {
               send_error(resp, 404, result.value("error", std::string("session not found")), "not_found");
               return;
             }
             send_json(resp, 200, result);
           });

  app_.post("/api/transport/listeners/:id/send", [this](uvcpp::uvcpp_web_request& req,
                                                        uvcpp::uvcpp_web_response& resp,
                                                        uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require_module(req, resp, "tcpudp", "debug");
    if (!principal) return;
    const std::string* id = req.param("id");
    if (!id || !transport_) {
      send_error(resp, 404, "listener not found", "not_found");
      return;
    }
    const auto body = parse_json(req, resp);
    if (!body) return;
    std::string error;
    if (!transport_->listener_send(*id, string_field(*body, "sessionId"),
                                   string_field(*body, "peerHost"),
                                   int_field(*body, "peerPort", 0),
                                   string_field(*body, "payload"),
                                   string_field(*body, "encoding", "text"), &error)) {
      send_error(resp, 400, error, "send_failed");
      return;
    }
    send_json(resp, 200, {{"ok", true}});
  });

  app_.del("/api/transport/listeners/:id/sessions/:sid",
           [this](uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
                  uvcpp::uvcpp_web_next) {
             const auto principal = auth_.require_module(req, resp, "tcpudp", "debug");
             if (!principal) return;
             const std::string* id = req.param("id");
             const std::string* sid = req.param("sid");
             std::string error;
             if (!id || !sid || !transport_ ||
                 !transport_->listener_close_session(*id, *sid, &error)) {
               send_error(resp, 400, error.empty() ? "session not found" : error, "close_failed");
               return;
             }
             send_json(resp, 200, {{"ok", true}});
           });

  app_.post("/api/tls/inspect", [this](uvcpp::uvcpp_web_request& req,
                                       uvcpp::uvcpp_web_response& resp,
                                       uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require_module(req, resp, "tls", "debug");
    if (!principal) return;
    const auto body = parse_json(req, resp);
    if (!body) return;
    std::string pem = string_field(*body, "pem");
    if (string_field(*body, "encoding", "text") == "base64") {
      const auto decoded = base64_decode(pem);
      if (!decoded) {
        send_error(resp, 400, "certificate is not valid base64", "invalid_certificate");
        return;
      }
      pem = *decoded;
    }
    std::string error;
    nlohmann::json info = inspect_certificate(pem, &error);
    if (!info.is_object()) {
      send_error(resp, 400, error.empty() ? "certificate parse failed" : error, "invalid_certificate");
      return;
    }
    metrics_.tls_inspections.fetch_add(1);
    add_history(*principal, "tls", "Certificate " + info.value("subject", std::string()), info);
    info["ok"] = true;
    send_json(resp, 200, info);
  });

  // Full-render preview: the SPA posts the captured HTML and receives a
  // short-lived token. /preview/:id is served with its own CSP sandbox so the
  // document runs scripts but stays on an opaque origin without uvscope
  // cookies. It is a deliberate, per-preview opt-in.
  app_.post("/api/preview", [this](uvcpp::uvcpp_web_request& req,
                                   uvcpp::uvcpp_web_response& resp,
                                   uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require_module(req, resp, "http", "debug");
    if (!principal) return;
    const auto body = parse_json(req, resp);
    if (!body) return;
    const std::string html = string_field(*body, "html");
    const std::string base_url = string_field(*body, "baseUrl");
    const std::string mode = lower(string_field(*body, "mode", "safe"));
    if (mode != "safe" && mode != "scripts") {
      send_error(resp, 400, "mode must be safe or scripts", "bad_request");
      return;
    }
    if (html.empty()) {
      send_error(resp, 400, "html is required", "bad_request");
      return;
    }
    if (html.size() > PreviewStore::kMaxDocumentBytes) {
      send_error(resp, 413, "preview document exceeds 8 MiB", "too_large");
      return;
    }
    if (!base_url.empty() && !safe_preview_base(base_url)) {
      send_error(resp, 400, "baseUrl must be an http(s) URL", "bad_request");
      return;
    }
    const auto id = previews_.put(html, base_url, mode);
    if (!id) {
      send_error(resp, 413, "preview document exceeds 8 MiB", "too_large");
      return;
    }
    send_json(resp, 200, {{"ok", true},
                          {"id", *id},
                          {"url", "/preview/" + *id},
                          {"expiresInSeconds", PreviewStore::kTtlSeconds}});
  });

  app_.del("/api/preview/:id", [this](uvcpp::uvcpp_web_request& req,
                                      uvcpp::uvcpp_web_response& resp,
                                      uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require_module(req, resp, "http", "debug");
    if (!principal) return;
    const std::string* id = req.param("id");
    if (id) previews_.remove(*id);
    send_json(resp, 200, {{"ok", true}});
  });

  app_.get("/preview/:id", [this](uvcpp::uvcpp_web_request& req,
                                  uvcpp::uvcpp_web_response& resp,
                                  uvcpp::uvcpp_web_next) {
    // Remote-resource variants (CSS, fonts, frames) live on /preview instead
    // of srcdoc because a srcdoc document inherits the SPA's stricter CSP.
    static const char* kSafeCsp =
        "sandbox; default-src 'none'; script-src 'none'; connect-src 'none'; "
        "style-src 'unsafe-inline' http: https:; "
        "img-src http: https: data: blob:; media-src http: https: data: blob:; "
        "font-src http: https: data:; "
        "frame-src http: https: data: blob:; "
        "object-src 'none'; base-uri http: https:; form-action 'none'; "
        "frame-ancestors 'self'";
    static const char* kScriptsCsp =
        "sandbox allow-scripts; default-src 'none'; "
        "script-src 'unsafe-inline' 'unsafe-eval' http: https: data: blob:; "
        "style-src 'unsafe-inline' http: https:; "
        "img-src http: https: data: blob:; media-src http: https: data: blob:; "
        "font-src http: https: data:; connect-src http: https: ws: wss:; "
        "frame-src http: https: data: blob:; worker-src blob: http: https:; "
        "object-src 'none'; base-uri http: https:; form-action 'none'; "
        "frame-ancestors 'self'";
    const std::string* id = req.param("id");
    const auto document = id ? previews_.get(*id) : std::nullopt;
    const char* csp = document && document->mode == "scripts" ? kScriptsCsp : kSafeCsp;
    resp.set_header("Content-Security-Policy", csp)
        .set_header("X-Frame-Options", "SAMEORIGIN")
        .set_header("Cache-Control", "no-store")
        .set_header("Referrer-Policy", "no-referrer");
    if (!document) {
      resp.status(404).body("preview not found or expired", "text/plain; charset=utf-8");
      resp.end();
      return;
    }
    resp.status(200).body(preview_document(document->html, document->base_url),
                          "text/html; charset=utf-8");
    resp.end();
  });
}

// ---------------------------------------------------------------------------
// SOCKS5 servers
// ---------------------------------------------------------------------------
void Application::register_socks5_routes() {
  app_.get("/api/socks5/servers", [this](uvcpp::uvcpp_web_request& req,
                                         uvcpp::uvcpp_web_response& resp,
                                         uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require_module(req, resp, "tcpudp", "debug");
    if (!principal) return;
    if (!socks5_) {
      send_error(resp, 503, "SOCKS5 runtime is unavailable", "unavailable");
      return;
    }
    send_json(resp, 200, socks5_->servers());
  });

  app_.post("/api/socks5/servers", [this](uvcpp::uvcpp_web_request& req,
                                          uvcpp::uvcpp_web_response& resp,
                                          uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require_module(req, resp, "tcpudp", "debug");
    if (!principal) return;
    if (!socks5_) {
      send_error(resp, 503, "SOCKS5 runtime is unavailable", "unavailable");
      return;
    }
    const auto body = parse_json(req, resp);
    if (!body) return;
    Socks5ServerConfig config;
    config.id = make_id();
    config.name = string_field(*body, "name");
    config.host = string_field(*body, "host", "127.0.0.1");
    config.port = int_field(*body, "port", 0);
    config.auto_start = bool_field(*body, "autoStart", false);
    config.username = string_field(*body, "username");
    const std::string password = string_field(*body, "password");
    config.auth_required = !config.username.empty() || !password.empty();
    if (config.host.empty() || config.port <= 0 || config.port > 65535) {
      send_error(resp, 400, "host and port are required", "bad_request");
      return;
    }
    if (config.auth_required && (config.username.empty() || password.empty())) {
      send_error(resp, 400, "username and password are both required", "bad_request");
      return;
    }
    if (config.auth_required) config.password_hash = password_hash(password);
    config.created_at = now_iso8601();

    std::string error;
    if (!socks5_->add_server(config, &error)) {
      send_error(resp, 400, error, "socks5_server_failed");
      return;
    }
    if (!store_.save_socks5_server(config, &error)) {
      socks5_->remove_server(config.id, nullptr);
      send_error(resp, 500, error, "persist_failed");
      return;
    }
    if (bool_field(*body, "start", true)) {
      if (!socks5_->start_server(config.id, &error)) {
        socks5_->remove_server(config.id, nullptr);
        store_.delete_socks5_server(config.id, nullptr);
        send_error(resp, 400, error, "socks5_server_failed");
        return;
      }
    }
    send_json(resp, 200, {{"ok", true}, {"server", config.id}});
  });

  app_.get("/api/socks5/servers/:id", [this](uvcpp::uvcpp_web_request& req,
                                             uvcpp::uvcpp_web_response& resp,
                                             uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require_module(req, resp, "tcpudp", "debug");
    if (!principal) return;
    const std::string* id = req.param("id");
    if (!id || !socks5_) {
      send_error(resp, 404, "SOCKS5 server not found", "not_found");
      return;
    }
    const auto result = socks5_->server(*id);
    if (!result.value("ok", false)) {
      send_error(resp, 404, result.value("error", std::string("SOCKS5 server not found")),
                 "not_found");
      return;
    }
    send_json(resp, 200, result);
  });

  app_.post("/api/socks5/servers/:id/start", [this](uvcpp::uvcpp_web_request& req,
                                                    uvcpp::uvcpp_web_response& resp,
                                                    uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require_module(req, resp, "tcpudp", "debug");
    if (!principal) return;
    const std::string* id = req.param("id");
    std::string error;
    if (!id || !socks5_ || !socks5_->start_server(*id, &error)) {
      send_error(resp, 400, error.empty() ? "SOCKS5 server not found" : error,
                 "socks5_server_failed");
      return;
    }
    send_json(resp, 200, {{"ok", true}});
  });

  app_.post("/api/socks5/servers/:id/stop", [this](uvcpp::uvcpp_web_request& req,
                                                   uvcpp::uvcpp_web_response& resp,
                                                   uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require_module(req, resp, "tcpudp", "debug");
    if (!principal) return;
    const std::string* id = req.param("id");
    std::string error;
    if (!id || !socks5_ || !socks5_->stop_server(*id, &error)) {
      send_error(resp, 400, error.empty() ? "SOCKS5 server not found" : error,
                 "socks5_server_failed");
      return;
    }
    send_json(resp, 200, {{"ok", true}});
  });

  app_.del("/api/socks5/servers/:id", [this](uvcpp::uvcpp_web_request& req,
                                             uvcpp::uvcpp_web_response& resp,
                                             uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require_module(req, resp, "tcpudp", "debug");
    if (!principal) return;
    const std::string* id = req.param("id");
    if (!id || !socks5_ || !socks5_->remove_server(*id, nullptr)) {
      send_error(resp, 404, "SOCKS5 server not found", "not_found");
      return;
    }
    store_.delete_socks5_server(*id, nullptr);
    send_json(resp, 200, {{"ok", true}});
  });

  app_.get("/api/socks5/servers/:id/events", [this](uvcpp::uvcpp_web_request& req,
                                                    uvcpp::uvcpp_web_response& resp,
                                                    uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require_module(req, resp, "tcpudp", "debug");
    if (!principal) return;
    const std::string* id = req.param("id");
    if (!id || !socks5_) {
      send_error(resp, 404, "SOCKS5 server not found", "not_found");
      return;
    }
    std::uint64_t after = 0;
    if (const std::string* value = req.query("after")) {
      after = std::strtoull(value->c_str(), nullptr, 10);
    }
    int limit = 200;
    if (const std::string* value = req.query("limit")) limit = std::atoi(value->c_str());
    const auto result = socks5_->server_events(*id, after, limit);
    if (!result.value("ok", false)) {
      send_error(resp, 404, result.value("error", std::string("SOCKS5 server not found")),
                 "not_found");
      return;
    }
    send_json(resp, 200, result);
  });

  app_.get("/api/socks5/servers/:id/sessions", [this](uvcpp::uvcpp_web_request& req,
                                                      uvcpp::uvcpp_web_response& resp,
                                                      uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require_module(req, resp, "tcpudp", "debug");
    if (!principal) return;
    const std::string* id = req.param("id");
    if (!id || !socks5_) {
      send_error(resp, 404, "SOCKS5 server not found", "not_found");
      return;
    }
    const auto result = socks5_->server_sessions(*id);
    if (!result.value("ok", false)) {
      send_error(resp, 404, result.value("error", std::string("SOCKS5 server not found")),
                 "not_found");
      return;
    }
    send_json(resp, 200, result);
  });
}

// ---------------------------------------------------------------------------
// Reverse proxies
// ---------------------------------------------------------------------------
nlohmann::json Application::proxy_view() const {
  nlohmann::json runtime = proxies_ ? proxies_->snapshot() : nlohmann::json::array();
  std::map<std::string, nlohmann::json> by_id;
  for (const auto& item : runtime) by_id[item.value("id", std::string())] = item;
  nlohmann::json out = nlohmann::json::array();
  for (const auto& config : store_.proxies()) {
    nlohmann::json item = {{"id", config.id},
                           {"name", config.name},
                           {"listenHost", config.listen_host},
                           {"listenPort", config.listen_port},
                           {"target", config.target},
                           {"autoStart", config.auto_start},
                           {"tls", config.tls},
                           {"certConfigured", !config.cert_path.empty()},
                           {"certPath", config.cert_path},
                           {"keyPath", config.key_path},
                           {"rules", proxy_rules_json(config.rules)},
                           {"running", false},
                           {"requests", 0},
                           {"errors", 0},
                           {"lastError", ""}};
    if (by_id.count(config.id)) item.update(by_id[config.id]);
    out.push_back(item);
  }
  return out;
}

void Application::register_proxy_routes() {
  app_.get("/api/proxies", [this](uvcpp::uvcpp_web_request& req,
                                  uvcpp::uvcpp_web_response& resp,
                                  uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require(req, resp, "dashboard");
    if (!principal) return;
    send_json(resp, 200, {{"ok", true}, {"proxies", proxy_view()}});
  });

  app_.post("/api/proxies", [this](uvcpp::uvcpp_web_request& req,
                                   uvcpp::uvcpp_web_response& resp,
                                   uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require_module(req, resp, "proxy", "debug");
    if (!principal) return;
    const auto body = parse_json(req, resp);
    if (!body) return;
    ProxyConfig config;
    config.id = string_field(*body, "id");
    if (config.id.empty()) config.id = make_id();
    config.name = string_field(*body, "name");
    config.listen_host = string_field(*body, "listenHost", "127.0.0.1");
    config.listen_port = int_field(*body, "listenPort", 0);
    config.target = string_field(*body, "target");
    config.auto_start = bool_field(*body, "autoStart", false);
    config.tls = bool_field(*body, "tls", false);
    config.cert_path = string_field(*body, "certPath");
    config.key_path = string_field(*body, "keyPath");
    if (config.name.empty()) config.name = "Proxy " + std::to_string(config.listen_port);
    if (config.listen_port < 1 || config.listen_port > 65535) {
      send_error(resp, 400, "listenPort must be between 1 and 65535", "bad_request");
      return;
    }
    if (!parse_url(config.target).valid) {
      send_error(resp, 400, "target must be a valid http:// or https:// URL", "bad_request");
      return;
    }
    const auto existing = store_.proxy(config.id);
    if (body->contains("rules")) {
      std::string rules_error;
      config.rules = proxy_rules_field(*body, &rules_error);
      if (!rules_error.empty()) {
        send_error(resp, 400, rules_error, "bad_request");
        return;
      }
    } else if (existing) {
      config.rules = existing->rules;
    }
    if (existing) config.created_at = existing->created_at;
    // Refuse accidental duplicate listeners.
    for (const auto& other : store_.proxies()) {
      if (other.id != config.id && other.listen_port == config.listen_port &&
          lower(other.listen_host) == lower(config.listen_host)) {
        send_error(resp, 409, "another proxy already uses this listen address", "port_in_use");
        return;
      }
    }
    std::string error;
    if (!proxies_->reload(config, &error)) {
      send_error(resp, 400, error, "proxy_save_failed");
      return;
    }
    const bool should_start = bool_field(*body, "start", false);
    // reload() restarts a running proxy in place; only start when it is still
    // stopped, otherwise this would report a spurious "already running".
    if (should_start && !proxies_->is_running(config.id) &&
        !proxies_->start(config.id, &error)) {
      send_json(resp, 200, {{"ok", true}, {"warning", error}, {"proxies", proxy_view()}});
      return;
    }
    send_json(resp, 200, {{"ok", true}, {"proxies", proxy_view()}});
  });

  app_.del("/api/proxies/:id", [this](uvcpp::uvcpp_web_request& req,
                                      uvcpp::uvcpp_web_response& resp,
                                      uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require_module(req, resp, "proxy", "debug");
    if (!principal) return;
    const std::string* id = req.param("id");
    if (id == nullptr) {
      send_error(resp, 400, "proxy id is required", "bad_request");
      return;
    }
    std::string error;
    proxies_->stop(*id, nullptr);
    if (!store_.delete_proxy(*id, &error)) {
      send_error(resp, 500, error);
      return;
    }
    send_json(resp, 200, {{"ok", true}, {"proxies", proxy_view()}});
  });

  app_.post("/api/proxies/:id/start", [this](uvcpp::uvcpp_web_request& req,
                                             uvcpp::uvcpp_web_response& resp,
                                             uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require_module(req, resp, "proxy", "debug");
    if (!principal) return;
    const std::string* id = req.param("id");
    if (id == nullptr) {
      send_error(resp, 400, "proxy id is required", "bad_request");
      return;
    }
    std::string error;
    if (!proxies_->start(*id, &error)) {
      send_error(resp, 502, error, "proxy_start_failed");
      return;
    }
    send_json(resp, 200, {{"ok", true}, {"proxies", proxy_view()}});
  });

  app_.post("/api/proxies/:id/stop", [this](uvcpp::uvcpp_web_request& req,
                                            uvcpp::uvcpp_web_response& resp,
                                            uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require_module(req, resp, "proxy", "debug");
    if (!principal) return;
    const std::string* id = req.param("id");
    if (id == nullptr) {
      send_error(resp, 400, "proxy id is required", "bad_request");
      return;
    }
    std::string error;
    if (!proxies_->stop(*id, &error)) {
      send_error(resp, 400, error, "proxy_stop_failed");
      return;
    }
    send_json(resp, 200, {{"ok", true}, {"proxies", proxy_view()}});
  });

  app_.get("/api/proxies/:id/logs", [this](uvcpp::uvcpp_web_request& req,
                                           uvcpp::uvcpp_web_response& resp,
                                           uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require(req, resp, "dashboard");
    if (!principal) return;
    const std::string* id = req.param("id");
    if (id == nullptr) {
      send_error(resp, 400, "proxy id is required", "bad_request");
      return;
    }
    std::uint64_t after = 0;
    if (const std::string* value = req.query("after")) after = std::strtoull(value->c_str(), nullptr, 10);
    int limit = 100;
    if (const std::string* value = req.query("limit")) limit = std::atoi(value->c_str());
    send_json(resp, 200, {{"ok", true}, {"logs", proxies_->logs(*id, after, limit)}});
  });
}

// ---------------------------------------------------------------------------
// Admin
// ---------------------------------------------------------------------------
void Application::register_admin_routes() {
  app_.get("/api/settings", [this](uvcpp::uvcpp_web_request& req,
                                   uvcpp::uvcpp_web_response& resp,
                                   uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require(req, resp, "admin.settings");
    if (!principal) return;
    send_json(resp, 200,
              {{"ok", true},
               {"modules", store_.module_states()},
               {"settings",
                {{"autoUpdateCheck", store_.setting("update.auto", "1") == "1"},
                 {"updateIntervalHours", std::atoi(store_.setting("update.interval", "24").c_str())},
                 {"repo", UVSCOPE_GITHUB_URL},
                 {"dataDir", store_.database_path().parent_path().string()}}}});
  });

  app_.put("/api/settings", [this](uvcpp::uvcpp_web_request& req,
                                   uvcpp::uvcpp_web_response& resp,
                                   uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require(req, resp, "admin.settings");
    if (!principal) return;
    const auto body = parse_json(req, resp);
    if (!body) return;
    std::string error;
    if (body->contains("autoUpdateCheck") && (*body)["autoUpdateCheck"].is_boolean()) {
      store_.set_setting("update.auto", (*body)["autoUpdateCheck"].get<bool>() ? "1" : "0", &error);
    }
    if (body->contains("updateIntervalHours") && (*body)["updateIntervalHours"].is_number_integer()) {
      const int hours = std::max(1, std::min(168, (*body)["updateIntervalHours"].get<int>()));
      store_.set_setting("update.interval", std::to_string(hours), &error);
    }
    if (body->contains("modules") && (*body)["modules"].is_object()) {
      for (auto it = (*body)["modules"].begin(); it != (*body)["modules"].end(); ++it) {
        if (!it.value().is_boolean()) continue;
        if (!module_id_valid(it.key()) || it.key() == "all") continue;
        if (!store_.set_module_enabled(it.key(), it.value().get<bool>(), &error)) {
          send_error(resp, 400, error, "settings_save_failed");
          return;
        }
      }
    }
    send_json(resp, 200, {{"ok", true}, {"modules", store_.module_states()}});
  });

  app_.get("/api/users", [this](uvcpp::uvcpp_web_request& req,
                                uvcpp::uvcpp_web_response& resp,
                                uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require(req, resp, "admin.users");
    if (!principal) return;
    nlohmann::json users = nlohmann::json::array();
    for (const auto& user : store_.list_users()) users.push_back(user_json(user));
    send_json(resp, 200, {{"ok", true}, {"users", users}});
  });

  app_.post("/api/users", [this](uvcpp::uvcpp_web_request& req,
                                 uvcpp::uvcpp_web_response& resp,
                                 uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require(req, resp, "admin.users");
    if (!principal) return;
    const auto body = parse_json(req, resp);
    if (!body) return;
    const std::string username = string_field(*body, "username");
    const std::string password = string_field(*body, "password");
    const std::string role = string_field(*body, "role", "viewer");
    std::string error;
    if (!store_.create_user(username, password, role, &error)) {
      send_error(resp, 400, error, "user_create_failed");
      return;
    }
    const auto created = store_.user_by_username(username);
    send_json(resp, 201, {{"ok", true}, {"user", created ? user_json(*created) : nlohmann::json()}});
  });

  app_.patch("/api/users/:id", [this](uvcpp::uvcpp_web_request& req,
                                      uvcpp::uvcpp_web_response& resp,
                                      uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require(req, resp, "admin.users");
    if (!principal) return;
    const std::string* id_text = req.param("id");
    if (id_text == nullptr) {
      send_error(resp, 400, "user id is required", "bad_request");
      return;
    }
    const auto body = parse_json(req, resp);
    if (!body) return;
    const std::int64_t id = std::strtoll(id_text->c_str(), nullptr, 10);
    std::optional<std::string> role;
    if (body->contains("role") && (*body)["role"].is_string()) role = (*body)["role"].get<std::string>();
    std::optional<bool> enabled;
    if (body->contains("enabled") && (*body)["enabled"].is_boolean()) {
      enabled = (*body)["enabled"].get<bool>();
    }
    if (id == principal->user.id && enabled && !*enabled) {
      send_error(resp, 400, "you cannot disable your own account", "self_disable");
      return;
    }
    std::optional<std::string> password;
    if (body->contains("password") && (*body)["password"].is_string() &&
        !(*body)["password"].get<std::string>().empty()) {
      password = (*body)["password"].get<std::string>();
    }
    std::string error;
    if (!store_.update_user(id, role, enabled, password, &error)) {
      send_error(resp, 400, error, "user_update_failed");
      return;
    }
    const auto updated = store_.user_by_id(id);
    send_json(resp, 200, {{"ok", true}, {"user", updated ? user_json(*updated) : nlohmann::json()}});
  });

  app_.del("/api/users/:id", [this](uvcpp::uvcpp_web_request& req,
                                    uvcpp::uvcpp_web_response& resp,
                                    uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require(req, resp, "admin.users");
    if (!principal) return;
    const std::string* id_text = req.param("id");
    if (id_text == nullptr) {
      send_error(resp, 400, "user id is required", "bad_request");
      return;
    }
    const std::int64_t id = std::strtoll(id_text->c_str(), nullptr, 10);
    if (id == principal->user.id) {
      send_error(resp, 400, "you cannot delete your own account", "self_delete");
      return;
    }
    std::string error;
    if (!store_.delete_user(id, &error)) {
      send_error(resp, 400, error, "user_delete_failed");
      return;
    }
    send_ok(resp);
  });

  app_.post("/api/reset", [this](uvcpp::uvcpp_web_request& req,
                                 uvcpp::uvcpp_web_response& resp,
                                 uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require(req, resp, "admin.reset");
    if (!principal) return;
    const auto body = parse_json(req, resp);
    if (!body) return;
    const std::string module = string_field(*body, "module", "all");
    if (!module_id_valid(module)) {
      send_error(resp, 400, "unknown module", "bad_request");
      return;
    }
    std::string error;
    if (module == "all") {
      if (proxies_) proxies_->stop_all();
      if (!store_.reset_database(&error)) {
        send_error(resp, 500, "database reset failed: " + error, "reset_failed");
        return;
      }
      proxies_.reset(new ProxyManager(store_));
      Auth::clear_cookie(resp);
      send_json(resp, 200, {{"ok", true}, {"setupRequired", true}});
      return;
    }
    if (module == "proxy" && proxies_) proxies_->stop_all();
    if (!store_.reset_module(module, &error)) {
      send_error(resp, 500, error, "reset_failed");
      return;
    }
    send_json(resp, 200, {{"ok", true}, {"module", module}});
  });
}

// ---------------------------------------------------------------------------
// Updates / about
// ---------------------------------------------------------------------------
nlohmann::json Application::github_get(const std::string& api_path, int* error_code,
                                       std::string* error) const {
  if (error_code) *error_code = 0;
  uvcpp::uvcpp_ssl_context ssl(uvcpp::tls_mode::CLIENT, uvcpp::tls_version::TLS_1_2);
  const std::string ca = find_ca_bundle(executable_dir(nullptr));
  if (!ca.empty()) ssl.load_ca_file(ca);
  ssl.set_verify_mode(uvcpp::tls_verify_mode::PEER_STRICT);
  uvcpp::uvcpp_http_client client;
  client.set_ssl_context(&ssl);
#if UVCPP_ZLIB_ENABLE
  client.set_compression_enabled(true);
#endif
  int rc = client.connect_wait("api.github.com", 443, 12000);
  if (rc != 0) {
    if (error_code) *error_code = rc;
    if (error) *error = uv_error_string(rc);
    return nullptr;
  }
  uvcpp::uvcpp_http_request request = uvcpp::uvcpp_http_request::make_get(api_path);
  request.set_header("user-agent", "uvscope/" UVSCOPE_VERSION);
  request.set_header("accept", "application/vnd.github+json");
  uvcpp::uvcpp_http_response response;
  rc = client.send_wait_ssl(request, response, 15000);
  if (rc != 0) {
    if (error_code) *error_code = rc;
    if (error) *error = uv_error_string(rc);
    return nullptr;
  }
  if (static_cast<int>(response.status_code) < 200 || static_cast<int>(response.status_code) >= 300) {
    if (error) {
      *error = "GitHub API returned HTTP " + std::to_string(static_cast<int>(response.status_code));
    }
    return nullptr;
  }
  nlohmann::json parsed = nlohmann::json::parse(response.body.to_string(), nullptr, false);
  if (parsed.is_discarded()) {
    if (error) *error = "GitHub API returned invalid JSON";
    return nullptr;
  }
  return parsed;
}

nlohmann::json Application::update_payload() {
  nlohmann::json out = {{"ok", true},
                        {"currentVersion", UVSCOPE_VERSION},
                        {"latestVersion", UVSCOPE_VERSION},
                        {"updateAvailable", false},
                        {"checkedAt", now_iso8601()},
                        {"repository", UVSCOPE_GITHUB_URL}};
  int rc = 0;
  std::string error;
  const std::string releases_path =
      "/repos/" UVSCOPE_GITHUB_OWNER "/" UVSCOPE_GITHUB_REPO "/releases/latest";
  nlohmann::json release = github_get(releases_path, &rc, &error);
  if (release.is_object()) {
    const std::string tag = release.value("tag_name", std::string());
    const std::string name = release.value("name", tag);
    const std::string notes = release.value("body", std::string());
    out["latestVersion"] = tag;
    out["releaseName"] = name;
    out["releaseNotes"] = notes.size() > 12000 ? notes.substr(0, 12000) : notes;
    out["publishedAt"] = release.value("published_at", std::string());
    out["htmlUrl"] = release.value("html_url", UVSCOPE_GITHUB_URL);
    out["updateAvailable"] = version_newer(tag, UVSCOPE_VERSION);
  } else {
    out["ok"] = false;
    out["error"] = error.empty() ? "update lookup failed" : error;
  }

  const std::string repo_path = "/repos/" UVSCOPE_GITHUB_OWNER "/" UVSCOPE_GITHUB_REPO;
  nlohmann::json repo = github_get(repo_path, &rc, &error);
  if (repo.is_object()) {
    out["starCount"] = repo.value("stargazers_count", 0);
    out["forks"] = repo.value("forks_count", 0);
    out["openIssues"] = repo.value("open_issues_count", 0);
    out["repoDescription"] = repo.value("description", std::string());
  }

  {
    std::lock_guard<std::mutex> lock(update_mutex_);
    update_cache_ = out;
    update_checked_at_ = now_unix();
    if (repo.is_object()) repo_cache_ = repo;
  }
  return out;
}

void Application::register_update_routes() {
  app_.get("/api/about", [this](uvcpp::uvcpp_web_request& req,
                                uvcpp::uvcpp_web_response& resp,
                                uvcpp::uvcpp_web_next) {
    const auto principal = auth_.require(req, resp, "about");
    if (!principal) return;
    std::int64_t stars = 0;
    {
      std::lock_guard<std::mutex> lock(update_mutex_);
      if (repo_cache_.is_object()) stars = repo_cache_.value("stargazers_count", 0LL);
    }
    send_json(resp, 200,
              {{"ok", true},
               {"name", "uvscope"},
               {"version", UVSCOPE_VERSION},
               {"libuvcpp", uvcpp::version_string()},
               {"libuv", libuv_version_string()},
               {"platform", platform_name()},
               {"license", "MIT"},
               {"repository", UVSCOPE_GITHUB_URL},
               {"issues", std::string(UVSCOPE_GITHUB_URL) + "/issues"},
               {"starCount", stars},
               {"description",
                "A libuvcpp-powered multi-protocol web debugging workbench and gateway"},
               {"modules", store_.module_states()},
               {"capabilities",
                {"HTTP/1.1 debugger", "HTTP/2 preference (experimental)", "HTTP/3 preference (experimental)",
                 "TCP probe", "UDP datagram probe", "WebSocket console", "TLS certificate decoder",
                 "SOAP/WSDL workbench", "HTTP reverse proxy", "SQLite auth and RBAC"}}});
  });

  app_.get("/api/update/check", [this](uvcpp::uvcpp_web_request& req,
                                       uvcpp::uvcpp_web_response& resp,
                                       uvcpp::uvcpp_web_next next) {
    const auto principal = auth_.require(req, resp, "about");
    if (!principal) return;
    const std::string* force = req.query("force");
    {
      std::lock_guard<std::mutex> lock(update_mutex_);
      if ((force == nullptr || *force != "1") && update_cache_.is_object() &&
          !update_cache_.empty() && now_unix() - update_checked_at_ < 900) {
        nlohmann::json cached = update_cache_;
        cached["cached"] = true;
        send_json(resp, 200, cached);
        return;
      }
    }
    post_job([this] { return JobResult{200, update_payload()}; }, &resp, next);
  });
}

// ---------------------------------------------------------------------------
// Jobs and history
// ---------------------------------------------------------------------------
void Application::post_job(std::function<JobResult()> work, uvcpp::uvcpp_web_response* resp,
                           uvcpp::uvcpp_web_next next,
                           std::function<void(JobResult&)> after) {
  auto* done = new JobResult();
  auto* req = new uvcpp::uvcpp_work();
  if (req->init() != 0) {
    delete req;
    delete done;
    send_error(*resp, 500, "could not initialize background work", "internal_error");
    return;
  }
  const int rc = req->queue_work(
      app_.loop(), [job = std::move(work), done](uvcpp::uvcpp_work*) {
        try {
          *done = job();
        } catch (const std::exception& ex) {
          done->status = 500;
          done->body = {{"ok", false}, {"error", std::string("background job failed: ") + ex.what()}};
        } catch (...) {
          done->status = 500;
          done->body = {{"ok", false}, {"error", "background job failed"}};
        }
      },
      [resp, next, done, after = std::move(after)](uvcpp::uvcpp_work* w, int status) {
        if (status != 0) {
          done->status = 500;
          done->body = {{"ok", false},
                        {"error", "background work failed: " + uv_error_string(status)}};
        }
        if (after) after(*done);
        std::string text = done->body.dump();
        resp->status(done->status).set_header("Cache-Control", "no-store").json_str(std::move(text));
        resp->end();
        next();
        delete done;
        delete w;
      });
  if (rc != 0) {
    delete req;
    delete done;
    send_error(*resp, 503, "could not queue background work", "work_queue_full");
  }
}

void Application::add_history(const Principal& principal, const std::string& kind,
                              const std::string& summary, const nlohmann::json& result) {
  HistoryItem item;
  item.user_id = principal.user.id;
  item.kind = kind;
  item.summary = summary;
  item.status = result.value("ok", false) ? "success" : "error";
  item.http_status = result.value("status", 0);
  if (result.contains("timing") && result["timing"].is_object()) {
    item.latency_ms = result["timing"].value("totalMs", 0LL);
  }
  item.size_bytes = result.value("bodySize", 0LL);
  nlohmann::json detail = {{"request", result.value("request", nlohmann::json::object())},
                           {"status", item.http_status},
                           {"error", result.value("error", std::string())},
                           {"timing", result.value("timing", nlohmann::json::object())}};
  if (kind == "tcp" || kind == "udp") {
    detail["sent"] = result.value("sent", nlohmann::json::object());
    detail["received"] = result.value("received", nlohmann::json::object());
  }
  if (kind == "tls") {
    detail["subject"] = result.value("subject", std::string());
    detail["issuer"] = result.value("issuer", std::string());
    detail["daysRemaining"] = result.value("daysRemaining", 0);
  }
  item.detail = detail.dump();
  store_.add_history(std::move(item));
}

// ---------------------------------------------------------------------------
// Self-check
// ---------------------------------------------------------------------------
int Application::self_check() {
  uvcpp::uvcpp_set_threadpool_size(0);
  int failures = 0;
  auto check = [&failures](bool ok, const char* name) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
  };
  std::printf("uvscope %s self-check\n", UVSCOPE_VERSION);
  check(uvcpp::version_string() != nullptr, "libuvcpp reports a version");

  const std::string hash = password_hash("correct horse battery staple");
  check(!hash.empty(), "password hash created");
  check(password_verify("correct horse battery staple", hash), "password verifies");
  check(!password_verify("wrong password", hash), "wrong password rejected");

  const auto temp = std::filesystem::temp_directory_path() / ("uvscope-selfcheck-" + random_hex(6));
  std::string error;
  check(ensure_directory(temp, &error), "temporary data directory created");
  Store store;
  check(store.open(temp / "uvscope.db", &error), "SQLite database opens");
  if (store.is_open()) {
    check(!store.initialized(), "fresh database needs setup");
    check(store.initialize_admin("admin", "admin-password", &error), "initial admin created");
    const auto user = store.verify_login("admin", "admin-password", &error);
    check(user.has_value(), "admin login verifies");
    if (user) {
      const std::string token = store.create_session(user->id);
      check(!token.empty(), "session created");
      const auto session = store.session_by_token(token);
      check(session.has_value() && session->user.username == "admin", "session resolves");
      store.delete_session(token);
    }
    store.close();
  } else {
    check(false, "SQLite database opens");
  }

  uvcpp::uvcpp_web_app local;
  local.set_host("127.0.0.1").set_port(0).set_access_log(false);
  local.get("/hello", [](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& resp,
                         uvcpp::uvcpp_web_next) {
    resp.json_str("{\"hello\":true}");
    resp.end();
  });
  const int rc = local.start();
  check(rc == 0, "ephemeral HTTP server starts");
  if (rc == 0) {
    HttpProbeInput input;
    input.url = "http://127.0.0.1:" + std::to_string(local.bound_port()) + "/hello";
    const nlohmann::json result = run_http_probe(input);
    check(result.value("ok", false) && result.value("status", 0) == 200,
          "libuvcpp HTTP self-request succeeds");
    local.stop();
    local.join();
  }

  std::error_code ec;
  std::filesystem::remove_all(temp, ec);
  std::printf("%s: %d failure(s)\n", failures == 0 ? "self-check OK" : "self-check FAILED", failures);
  return failures == 0 ? 0 : 1;
}

}  // namespace uvscope
