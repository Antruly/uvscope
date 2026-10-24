#include "store.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <sstream>

#include <nlohmann/json.hpp>

#include "command_seed.h"
#include "crypto.h"
#include "util.h"

namespace uvscope {
namespace {

constexpr const char* kModules[] = {"http", "websocket", "tcpudp", "soap", "tls", "proxy",
                                    "terminal"};
constexpr int kSessionTtlSeconds = 7 * 24 * 60 * 60;

bool is_known_module(const std::string& module) {
  for (const char* known : kModules) {
    if (module == known) return true;
  }
  return false;
}

std::string module_setting_key(const std::string& module) {
  return "module." + module + ".enabled";
}

bool module_default_enabled(const std::string& module) {
  // A terminal is a remote shell; it stays off until an administrator opts in.
  return module != "terminal";
}

std::string feature_defaults() {
  return R"({"http":true,"websocket":true,"tcpudp":true,"soap":true,"tls":true,"proxy":true,"terminal":false})";
}

nlohmann::json proxy_upstreams_to_json(const std::vector<ProxyUpstream>& upstreams) {
  nlohmann::json out = nlohmann::json::array();
  for (const auto& upstream : upstreams) {
    out.push_back({{"url", upstream.url}, {"weight", upstream.weight}});
  }
  return out;
}

std::vector<ProxyUpstream> proxy_upstreams_from_json(const std::string& text) {
  std::vector<ProxyUpstream> upstreams;
  const auto parsed = nlohmann::json::parse(text, nullptr, false);
  if (!parsed.is_array()) return upstreams;
  for (const auto& item : parsed) {
    if (!item.is_object()) continue;
    ProxyUpstream upstream;
    upstream.url = item.value("url", std::string());
    upstream.weight = item.value("weight", 1);
    if (upstream.url.empty()) continue;
    if (upstream.weight < 0) upstream.weight = 0;
    upstreams.push_back(std::move(upstream));
  }
  return upstreams;
}

nlohmann::json proxy_rules_to_json(const std::vector<ProxyRule>& rules) {
  nlohmann::json out = nlohmann::json::array();
  for (const auto& rule : rules) {
    out.push_back({{"id", rule.id},
                   {"name", rule.name},
                   {"enabled", rule.enabled},
                   {"matchType", rule.match_type},
                   {"pattern", rule.pattern},
                   {"method", rule.method},
                   {"host", rule.host},
                   {"headerName", rule.header_name},
                   {"headerValue", rule.header_value},
                   {"action", rule.action},
                   {"status", rule.status},
                   {"destination", rule.destination},
                   {"preserveQuery", rule.preserve_query}});
  }
  return out;
}

std::vector<ProxyRule> proxy_rules_from_json(const std::string& text) {
  std::vector<ProxyRule> rules;
  const auto parsed = nlohmann::json::parse(text, nullptr, false);
  if (!parsed.is_array()) return rules;
  for (const auto& item : parsed) {
    if (!item.is_object()) continue;
    ProxyRule rule;
    rule.id = item.value("id", std::string());
    rule.name = item.value("name", std::string());
    rule.enabled = item.value("enabled", true);
    rule.match_type = item.value("matchType", std::string("prefix"));
    rule.pattern = item.value("pattern", std::string());
    rule.method = item.value("method", std::string());
    rule.host = item.value("host", std::string());
    rule.header_name = item.value("headerName", std::string());
    rule.header_value = item.value("headerValue", std::string());
    rule.action = item.value("action", std::string("redirect"));
    rule.status = item.value("status", 302);
    rule.destination = item.value("destination", std::string());
    rule.preserve_query = item.value("preserveQuery", true);
    if (rule.id.empty()) continue;
    if (rule.pattern.empty() && rule.method.empty() && rule.host.empty() &&
        rule.header_name.empty()) {
      continue;
    }
    rules.push_back(std::move(rule));
  }
  return rules;
}

}  // namespace

Store::~Store() { close(); }

void Store::set_error(const std::string& message) const { last_error_ = message; }

bool Store::open(const std::filesystem::path& database_path, std::string* error) {
  close();
  std::error_code ec;
  std::filesystem::create_directories(database_path.parent_path(), ec);
  if (ec) {
    if (error) *error = "cannot create data directory: " + ec.message();
    return false;
  }
  database_path_ = std::filesystem::absolute(database_path, ec);
  if (ec) database_path_ = database_path;

  // A restore uploaded through /api/backup/restore is staged next to the live
  // database and takes effect here, before anything opens it. Keep the previous
  // file for manual recovery.
  std::filesystem::path restore_path = database_path_;
  restore_path += ".restore";
  if (std::filesystem::exists(restore_path, ec)) {
    std::filesystem::remove(database_path_.string() + "-wal", ec);
    std::filesystem::remove(database_path_.string() + "-shm", ec);
    if (std::filesystem::exists(database_path_, ec)) {
      const std::string stamp = std::to_string(now_unix());
      std::filesystem::rename(database_path_,
                              database_path_.string() + ".pre-restore-" + stamp, ec);
    }
    std::filesystem::rename(restore_path, database_path_, ec);
  }

  db_.set_timeout_ms(5000);
  const std::string url = "sqlite://" + database_path_.generic_string();
  const auto status = db_.open(url);
  if (!uvcpp::uvcpp_db_ok(status)) {
    set_error(db_.last_error());
    if (error) *error = last_error_;
    return false;
  }
  if (!migrate(error)) {
    db_.close();
    return false;
  }
  exec("PRAGMA foreign_keys = ON", nullptr);
  exec("PRAGMA journal_mode = WAL", nullptr);
  exec("PRAGMA synchronous = NORMAL", nullptr);
  exec("PRAGMA busy_timeout = 5000", nullptr);
  cleanup_sessions();
  return true;
}

void Store::close() {
  if (db_.is_open()) db_.close();
}

bool Store::exec(const std::string& sql, std::string* error) {
  const auto status = db_.execute(sql);
  if (!uvcpp::uvcpp_db_ok(status)) {
    set_error(db_.last_error() + " [SQL: " + sql.substr(0, 120) + "]");
    if (error) *error = last_error_;
    return false;
  }
  return true;
}

bool Store::exec(const std::string& sql, const uvcpp::uvcpp_db_params& params,
                 std::string* error) {
  const auto status = db_.execute(sql, params);
  if (!uvcpp::uvcpp_db_ok(status)) {
    set_error(db_.last_error());
    if (error) *error = last_error_;
    return false;
  }
  return true;
}

std::optional<uvcpp::uvcpp_db_table> Store::query(
    const std::string& sql, const uvcpp::uvcpp_db_params& params, std::string* error) const {
  uvcpp::uvcpp_db_table table;
  const auto status = params.empty() ? db_.query(sql, &table) : db_.query(sql, params, &table);
  if (!uvcpp::uvcpp_db_ok(status)) {
    set_error(db_.last_error());
    if (error) *error = last_error_;
    return std::nullopt;
  }
  return table;
}

bool Store::migrate(std::string* error) {
  const std::vector<const char*> statements = {
      R"(CREATE TABLE IF NOT EXISTS meta (
           key TEXT PRIMARY KEY,
           value TEXT NOT NULL
         ))",
      R"(CREATE TABLE IF NOT EXISTS users (
           id INTEGER PRIMARY KEY AUTOINCREMENT,
           username TEXT NOT NULL UNIQUE,
           password_hash TEXT NOT NULL,
           role TEXT NOT NULL DEFAULT 'viewer',
           enabled INTEGER NOT NULL DEFAULT 1,
           created_at TEXT NOT NULL,
           last_login TEXT
         ))",
      R"(CREATE TABLE IF NOT EXISTS sessions (
           token_hash TEXT PRIMARY KEY,
           user_id INTEGER NOT NULL,
           created_at TEXT NOT NULL,
           expires_at INTEGER NOT NULL
         ))",
      R"(CREATE TABLE IF NOT EXISTS audit (
           id INTEGER PRIMARY KEY AUTOINCREMENT,
           at TEXT NOT NULL,
           user_id INTEGER NOT NULL DEFAULT 0,
           username TEXT NOT NULL DEFAULT '',
           action TEXT NOT NULL,
           target TEXT NOT NULL DEFAULT '',
           detail TEXT NOT NULL DEFAULT '',
           ip TEXT NOT NULL DEFAULT ''
         ))",
      R"(CREATE INDEX IF NOT EXISTS idx_audit_at ON audit(id DESC))",
      R"(CREATE TABLE IF NOT EXISTS settings (
           key TEXT PRIMARY KEY,
           value TEXT NOT NULL,
           updated_at TEXT NOT NULL
         ))",
      R"(CREATE TABLE IF NOT EXISTS history (
           id INTEGER PRIMARY KEY AUTOINCREMENT,
           user_id INTEGER NOT NULL DEFAULT 0,
           kind TEXT NOT NULL,
           summary TEXT NOT NULL,
           status TEXT NOT NULL,
           http_status INTEGER NOT NULL DEFAULT 0,
           latency_ms INTEGER NOT NULL DEFAULT 0,
           size_bytes INTEGER NOT NULL DEFAULT 0,
           detail TEXT NOT NULL DEFAULT '{}',
           created_at TEXT NOT NULL
         ))",
      R"(CREATE INDEX IF NOT EXISTS idx_history_kind_created
           ON history(kind, created_at DESC))",
      R"(CREATE TABLE IF NOT EXISTS proxies (
           id TEXT PRIMARY KEY,
           name TEXT NOT NULL,
           listen_host TEXT NOT NULL,
           listen_port INTEGER NOT NULL,
           target TEXT NOT NULL,
           auto_start INTEGER NOT NULL DEFAULT 0,
           tls INTEGER NOT NULL DEFAULT 0,
           cert_path TEXT NOT NULL DEFAULT '',
           key_path TEXT NOT NULL DEFAULT '',
           rules_json TEXT NOT NULL DEFAULT '[]',
           upstreams_json TEXT NOT NULL DEFAULT '[]',
           retries INTEGER NOT NULL DEFAULT 0,
           connect_timeout_ms INTEGER NOT NULL DEFAULT 15000,
           request_timeout_ms INTEGER NOT NULL DEFAULT 30000,
           workers INTEGER NOT NULL DEFAULT 4,
           breaker_threshold INTEGER NOT NULL DEFAULT 5,
           breaker_cooldown_ms INTEGER NOT NULL DEFAULT 30000,
           isolated INTEGER NOT NULL DEFAULT 0,
           created_at TEXT NOT NULL,
           updated_at TEXT NOT NULL
         ))",
      R"(CREATE TABLE IF NOT EXISTS meta_schema (
           version INTEGER NOT NULL
         ))",
      R"(CREATE TABLE IF NOT EXISTS terminal_commands (
           id INTEGER PRIMARY KEY AUTOINCREMENT,
           command TEXT NOT NULL,
           name_en TEXT NOT NULL,
           name_zh TEXT NOT NULL,
           description_en TEXT NOT NULL DEFAULT '',
           description_zh TEXT NOT NULL DEFAULT '',
           category TEXT NOT NULL,
           platform TEXT NOT NULL,
           tags TEXT NOT NULL DEFAULT '',
           quick INTEGER NOT NULL DEFAULT 0,
           builtin INTEGER NOT NULL DEFAULT 0,
           owner_id INTEGER NOT NULL DEFAULT 0,
           created_at TEXT NOT NULL,
           updated_at TEXT NOT NULL
         ))",
      R"(CREATE INDEX IF NOT EXISTS idx_terminal_commands_platform
           ON terminal_commands(platform, category))"};
  for (const char* sql : statements) {
    if (!exec(sql, error)) return false;
  }
  // 0.3.x stores proxy routing rules in the proxy row. Older databases get the
  // column lazily; CREATE TABLE above already includes it for new files.
  bool has_rules_column = false;
  if (const auto columns = query("PRAGMA table_info(proxies)")) {
    for (const auto& row : columns->rows()) {
      if (row["name"].to_text() == "rules_json") {
        has_rules_column = true;
        break;
      }
    }
  }
  if (!has_rules_column) {
    if (!exec("ALTER TABLE proxies ADD COLUMN rules_json TEXT NOT NULL DEFAULT '[]'", error)) {
      return false;
    }
  }
  bool has_upstreams_column = false;
  bool has_retries_column = false;
  bool has_connect_timeout_column = false;
  bool has_request_timeout_column = false;
  bool has_workers_column = false;
  bool has_breaker_threshold_column = false;
  bool has_breaker_cooldown_column = false;
  bool has_isolated_column = false;
  if (const auto columns = query("PRAGMA table_info(proxies)")) {
    for (const auto& row : columns->rows()) {
      if (row["name"].to_text() == "upstreams_json") has_upstreams_column = true;
      if (row["name"].to_text() == "retries") has_retries_column = true;
      if (row["name"].to_text() == "connect_timeout_ms") has_connect_timeout_column = true;
      if (row["name"].to_text() == "request_timeout_ms") has_request_timeout_column = true;
      if (row["name"].to_text() == "workers") has_workers_column = true;
      if (row["name"].to_text() == "breaker_threshold") has_breaker_threshold_column = true;
      if (row["name"].to_text() == "breaker_cooldown_ms") has_breaker_cooldown_column = true;
      if (row["name"].to_text() == "isolated") has_isolated_column = true;
    }
  }
  if (!has_upstreams_column) {
    if (!exec("ALTER TABLE proxies ADD COLUMN upstreams_json TEXT NOT NULL DEFAULT '[]'", error)) {
      return false;
    }
  }
  if (!has_retries_column) {
    if (!exec("ALTER TABLE proxies ADD COLUMN retries INTEGER NOT NULL DEFAULT 0", error)) {
      return false;
    }
  }
  if (!has_connect_timeout_column) {
    if (!exec("ALTER TABLE proxies ADD COLUMN connect_timeout_ms INTEGER NOT NULL DEFAULT 15000",
              error)) {
      return false;
    }
  }
  if (!has_request_timeout_column) {
    if (!exec("ALTER TABLE proxies ADD COLUMN request_timeout_ms INTEGER NOT NULL DEFAULT 30000",
              error)) {
      return false;
    }
  }
  if (!has_workers_column) {
    if (!exec("ALTER TABLE proxies ADD COLUMN workers INTEGER NOT NULL DEFAULT 4", error)) {
      return false;
    }
  }
  if (!has_breaker_threshold_column) {
    if (!exec("ALTER TABLE proxies ADD COLUMN breaker_threshold INTEGER NOT NULL DEFAULT 5",
              error)) {
      return false;
    }
  }
  if (!has_breaker_cooldown_column) {
    if (!exec("ALTER TABLE proxies ADD COLUMN breaker_cooldown_ms INTEGER NOT NULL DEFAULT 30000",
              error)) {
      return false;
    }
  }
  if (!has_isolated_column) {
    if (!exec("ALTER TABLE proxies ADD COLUMN isolated INTEGER NOT NULL DEFAULT 0", error)) {
      return false;
    }
  }
  if (setting("terminal.commands.seeded", "0") != "1") {
    exec("BEGIN", nullptr);
    bool seeded = true;
    for (const auto& seed : terminal_command_seeds()) {
      if (!exec("INSERT INTO terminal_commands(command,name_en,name_zh,description_en,"
                "description_zh,category,platform,tags,quick,builtin,owner_id,created_at,"
                "updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
                {seed.command, seed.name_en, seed.name_zh, seed.description_en,
                 seed.description_zh, seed.category, seed.platform, seed.tags,
                 seed.quick ? 1 : 0, 1, 0, now_iso8601(), now_iso8601()},
                error)) {
        seeded = false;
        break;
      }
    }
    if (!seeded) {
      exec("ROLLBACK", nullptr);
      return false;
    }
    exec("COMMIT", nullptr);
    set_setting("terminal.commands.seeded", "1", nullptr);
  }
  exec("INSERT INTO meta(key, value) VALUES('feature.defaults', ?) ON CONFLICT(key) DO NOTHING",
       {feature_defaults()}, nullptr);
  exec("INSERT INTO meta(key, value) VALUES('schema.version', '1') ON CONFLICT(key) DO NOTHING",
       nullptr);
  return true;
}

bool Store::initialized() const {
  const auto table = query("SELECT COUNT(*) AS c FROM users WHERE role = 'admin'");
  if (!table) return false;
  return table->value("c").to_int64() > 0;
}

bool Store::initialize_admin(const std::string& username, const std::string& password,
                             std::string* error) {
  if (initialized()) {
    if (error) *error = "uvscope is already initialized";
    return false;
  }
  if (username.size() < 3 || username.size() > 64) {
    if (error) *error = "username must be 3-64 characters";
    return false;
  }
  if (password.size() < 8 || password.size() > 256) {
    if (error) *error = "password must be 8-256 characters";
    return false;
  }
  if (!uvcpp::uvcpp_db_ok(db_.begin())) {
    if (error) *error = db_.last_error();
    return false;
  }
  const std::string hash = password_hash(password);
  const std::string now = now_iso8601();
  int64_t affected = 0;
  const auto st = db_.execute(
      "INSERT INTO users(username, password_hash, role, enabled, created_at) VALUES(?, ?, 'admin', 1, ?)",
      {username, hash, now}, &affected);
  if (!uvcpp::uvcpp_db_ok(st)) {
    db_.rollback();
    if (error) *error = db_.last_error();
    return false;
  }
  const auto inserted = db_.execute(
      "INSERT INTO meta(key, value) VALUES('initialized', '1') "
      "ON CONFLICT(key) DO UPDATE SET value='1'");
  if (!uvcpp::uvcpp_db_ok(inserted)) {
    db_.rollback();
    if (error) *error = db_.last_error();
    return false;
  }
  if (!uvcpp::uvcpp_db_ok(db_.commit())) {
    if (error) *error = db_.last_error();
    return false;
  }
  return true;
}

bool Store::reset_database(std::string* error) {
  const auto path = database_path_;
  db_.close();
  std::error_code ec;
  std::filesystem::remove(path, ec);
  std::filesystem::remove(path.string() + "-wal", ec);
  std::filesystem::remove(path.string() + "-shm", ec);
  return open(path, error);
}

std::optional<User> Store::verify_login(const std::string& username,
                                        const std::string& password,
                                        std::string* error) {
  uvcpp::uvcpp_db_table table;
  const auto st = db_.query("SELECT id, username, password_hash, role, enabled, created_at, "
                            "COALESCE(last_login, '') AS last_login FROM users WHERE username = ?",
                            {username}, &table);
  if (!uvcpp::uvcpp_db_ok(st)) {
    if (error) *error = db_.last_error();
    return std::nullopt;
  }
  if (table.empty()) return std::nullopt;
  const auto& row = table.rows().front();
  if (!row["enabled"].to_bool()) return std::nullopt;
  if (!password_verify(password, row["password_hash"].to_text())) return std::nullopt;
  User user = row_to_user(row);
  if (password_needs_rehash(row["password_hash"].to_text())) {
    const std::string fresh = password_hash(password);
    db_.execute("UPDATE users SET password_hash = ? WHERE id = ?", {fresh, user.id}, nullptr);
  }
  return user;
}

std::optional<User> Store::user_by_id(std::int64_t id) const {
  const auto table = query("SELECT id, username, role, enabled, created_at, "
                           "COALESCE(last_login, '') AS last_login FROM users WHERE id = ?",
                           {id});
  if (!table || table->empty()) return std::nullopt;
  return row_to_user(table->rows().front());
}

std::optional<User> Store::user_by_username(const std::string& username) const {
  const auto table = query("SELECT id, username, role, enabled, created_at, "
                           "COALESCE(last_login, '') AS last_login FROM users WHERE username = ?",
                           {username});
  if (!table || table->empty()) return std::nullopt;
  return row_to_user(table->rows().front());
}

std::string Store::create_session(std::int64_t user_id) {
  cleanup_sessions();
  const std::string token = random_hex(32);
  const std::string hash = sha256_hex(token);
  const std::int64_t expires = now_unix() + kSessionTtlSeconds;
  db_.execute("INSERT INTO sessions(token_hash, user_id, created_at, expires_at) VALUES(?, ?, ?, ?)",
              {hash, user_id, now_iso8601(), expires}, nullptr);
  return token;
}

std::optional<Session> Store::session_by_token(const std::string& token) const {
  if (token.empty()) return std::nullopt;
  const std::string hash = sha256_hex(token);
  const auto table = query(
      "SELECT u.id, u.username, u.role, u.enabled, u.created_at, "
      "COALESCE(u.last_login, '') AS last_login, s.expires_at "
      "FROM sessions s JOIN users u ON u.id = s.user_id "
      "WHERE s.token_hash = ? AND s.expires_at > ?",
      {hash, now_unix()});
  if (!table || table->empty()) return std::nullopt;
  const auto& row = table->rows().front();
  if (!row["enabled"].to_bool()) return std::nullopt;
  Session session;
  session.user = row_to_user(row);
  session.expires_at = row["expires_at"].to_int64();
  return session;
}

void Store::delete_session(const std::string& token) {
  if (token.empty()) return;
  db_.execute("DELETE FROM sessions WHERE token_hash = ?", {sha256_hex(token)}, nullptr);
}

void Store::delete_sessions_for_user(std::int64_t user_id) {
  exec("DELETE FROM sessions WHERE user_id = ?", {user_id}, nullptr);
}

bool Store::backup_database(const std::string& path, std::string* error) {
  if (path.empty()) {
    if (error) *error = "backup path is required";
    return false;
  }
  return exec("VACUUM INTO ?", {path}, error);
}

void Store::cleanup_sessions() {
  exec("DELETE FROM sessions WHERE expires_at <= " + std::to_string(now_unix()), nullptr);
}

std::vector<SessionInfo> Store::active_sessions() const {
  std::vector<SessionInfo> out;
  const auto table = query(
      "SELECT s.token_hash, s.user_id, u.username, u.role, s.created_at, s.expires_at "
      "FROM sessions s JOIN users u ON u.id = s.user_id "
      "WHERE s.expires_at > ? ORDER BY s.created_at DESC",
      {now_unix()});
  if (!table) return out;
  for (const auto& row : table->rows()) {
    SessionInfo info;
    info.id = row["token_hash"].to_text();
    info.user_id = row["user_id"].to_int64();
    info.username = row["username"].to_text();
    info.role = row["role"].to_text();
    info.created_at = row["created_at"].to_text();
    info.expires_at = row["expires_at"].to_int64();
    out.push_back(std::move(info));
  }
  return out;
}

bool Store::delete_session_by_hash(const std::string& hash) {
  if (hash.empty()) return false;
  return exec("DELETE FROM sessions WHERE token_hash = ?", {hash}, nullptr);
}

void Store::audit(std::int64_t user_id, const std::string& username,
                  const std::string& action, const std::string& target,
                  const std::string& detail, const std::string& ip) {
  const std::string at = now_iso8601();
  db_.execute(
      "INSERT INTO audit(at,user_id,username,action,target,detail,ip) "
      "VALUES(?,?,?,?,?,?,?)",
      {at, user_id, username, action, target, detail, ip}, nullptr);
  // Keep the log bounded; the oldest rows are the least useful for a local tool.
  exec("DELETE FROM audit WHERE id <= (SELECT COALESCE(MAX(id),0) - 10000 FROM audit)",
       {}, nullptr);
}

std::vector<AuditEntry> Store::audit_entries(std::int64_t after, int limit) const {
  std::vector<AuditEntry> out;
  if (limit <= 0 || limit > 500) limit = 200;
  const auto table = query(
      "SELECT id,at,user_id,username,action,target,detail,ip FROM audit "
      "WHERE id > ? ORDER BY id ASC LIMIT ?",
      {after, limit});
  if (!table) return out;
  for (const auto& row : table->rows()) {
    AuditEntry entry;
    entry.id = row["id"].to_int64();
    entry.at = row["at"].to_text();
    entry.user_id = row["user_id"].to_int64();
    entry.username = row["username"].to_text();
    entry.action = row["action"].to_text();
    entry.target = row["target"].to_text();
    entry.detail = row["detail"].to_text();
    entry.ip = row["ip"].to_text();
    out.push_back(std::move(entry));
  }
  return out;
}

std::vector<User> Store::list_users() const {
  std::vector<User> users;
  const auto table = query("SELECT id, username, role, enabled, created_at, "
                           "COALESCE(last_login, '') AS last_login FROM users ORDER BY id ASC");
  if (!table) return users;
  for (const auto& row : table->rows()) users.push_back(row_to_user(row));
  return users;
}

bool Store::create_user(const std::string& username, const std::string& password,
                        const std::string& role, std::string* error) {
  if (username.size() < 3 || username.size() > 64) {
    if (error) *error = "username must be 3-64 characters";
    return false;
  }
  if (password.size() < 8) {
    if (error) *error = "password must be at least 8 characters";
    return false;
  }
  if (!role_valid(role)) {
    if (error) *error = "invalid role";
    return false;
  }
  const auto st = db_.execute(
      "INSERT INTO users(username, password_hash, role, enabled, created_at) VALUES(?, ?, ?, 1, ?)",
      {username, password_hash(password), role, now_iso8601()}, nullptr);
  if (!uvcpp::uvcpp_db_ok(st)) {
    if (error) *error = db_.last_error().empty() ? "username already exists" : db_.last_error();
    return false;
  }
  return true;
}

bool Store::update_user(std::int64_t id, const std::optional<std::string>& role,
                        const std::optional<bool>& enabled,
                        const std::optional<std::string>& new_password, std::string* error) {
  const auto existing = user_by_id(id);
  if (!existing) {
    if (error) *error = "user not found";
    return false;
  }
  if (role && !role_valid(*role)) {
    if (error) *error = "invalid role";
    return false;
  }
  if (role && existing->role == "admin" && *role != "admin") {
    const auto admins = query("SELECT COUNT(*) AS c FROM users WHERE role = 'admin' AND enabled = 1");
    if (!admins || admins->value("c").to_int64() <= 1) {
      if (error) *error = "cannot demote the last administrator";
      return false;
    }
  }
  if (enabled && !*enabled && existing->role == "admin") {
    const auto admins = query("SELECT COUNT(*) AS c FROM users WHERE role = 'admin' AND enabled = 1");
    if (!admins || admins->value("c").to_int64() <= 1) {
      if (error) *error = "cannot disable the last administrator";
      return false;
    }
  }
  if (role) {
    if (!exec("UPDATE users SET role = ? WHERE id = ?", {*role, id}, error)) return false;
  }
  if (enabled) {
    if (!exec("UPDATE users SET enabled = ? WHERE id = ?", {*enabled ? 1 : 0, id}, error)) return false;
  }
  if (new_password) {
    if (new_password->size() < 8) {
      if (error) *error = "password must be at least 8 characters";
      return false;
    }
    if (!exec("UPDATE users SET password_hash = ? WHERE id = ?",
              {password_hash(*new_password), id}, error)) return false;
    exec("DELETE FROM sessions WHERE user_id = ?", {id}, nullptr);
  }
  return true;
}

bool Store::delete_user(std::int64_t id, std::string* error) {
  const auto existing = user_by_id(id);
  if (!existing) {
    if (error) *error = "user not found";
    return false;
  }
  if (existing->role == "admin") {
    const auto admins = query("SELECT COUNT(*) AS c FROM users WHERE role = 'admin'");
    if (!admins || admins->value("c").to_int64() <= 1) {
      if (error) *error = "cannot delete the last administrator";
      return false;
    }
  }
  if (!uvcpp::uvcpp_db_ok(db_.begin())) {
    if (error) *error = db_.last_error();
    return false;
  }
  if (!exec("DELETE FROM sessions WHERE user_id = ?", {id}, error) ||
      !exec("DELETE FROM users WHERE id = ?", {id}, error)) {
    db_.rollback();
    return false;
  }
  if (!uvcpp::uvcpp_db_ok(db_.commit())) {
    if (error) *error = db_.last_error();
    return false;
  }
  return true;
}

bool Store::change_password(std::int64_t user_id, const std::string& new_password,
                            std::string* error) {
  if (new_password.size() < 8) {
    if (error) *error = "password must be at least 8 characters";
    return false;
  }
  return exec("UPDATE users SET password_hash = ? WHERE id = ?",
              {password_hash(new_password), user_id}, error);
}

bool Store::set_last_login(std::int64_t user_id) {
  return exec("UPDATE users SET last_login = ? WHERE id = ?", {now_iso8601(), user_id}, nullptr);
}

std::string Store::setting(const std::string& key, const std::string& fallback) const {
  const auto table = query("SELECT value FROM settings WHERE key = ?", {key});
  if (!table || table->empty()) return fallback;
  return table->value("value").to_text();
}

bool Store::set_setting(const std::string& key, const std::string& value, std::string* error) {
  return exec("INSERT INTO settings(key, value, updated_at) VALUES(?, ?, ?) "
              "ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at",
              {key, value, now_iso8601()}, error);
}

std::map<std::string, bool> Store::module_states() const {
  std::map<std::string, bool> out;
  for (const char* module : kModules) {
    const std::string value =
        setting(module_setting_key(module), module_default_enabled(module) ? "1" : "0");
    out[module] = value != "0" && value != "false";
  }
  return out;
}

bool Store::module_enabled(const std::string& module) const {
  if (!is_known_module(module)) return false;
  const std::string value =
      setting(module_setting_key(module), module_default_enabled(module) ? "1" : "0");
  return value != "0" && value != "false";
}

bool Store::set_module_enabled(const std::string& module, bool enabled, std::string* error) {
  if (!is_known_module(module)) {
    if (error) *error = "unknown module";
    return false;
  }
  return set_setting(module_setting_key(module), enabled ? "1" : "0", error);
}

bool Store::reset_module(const std::string& module, std::string* error) {
  if (module == "history" || module == "all") return clear_all_history(error);
  if (module == "proxy") {
    if (!exec("DELETE FROM proxies", error)) return false;
    set_setting("proxy.logs", "[]", nullptr);
    return true;
  }
  return exec("DELETE FROM history WHERE kind = ?", {module}, error);
}

void Store::add_history(HistoryItem item) {
  if (item.created_at.empty()) item.created_at = now_iso8601();
  if (item.detail.empty()) item.detail = "{}";
  if (item.detail.size() > 65536) item.detail.resize(65536);
  db_.execute("INSERT INTO history(user_id, kind, summary, status, http_status, latency_ms, "
              "size_bytes, detail, created_at) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)",
              {item.user_id, item.kind, item.summary, item.status, item.http_status,
               item.latency_ms, item.size_bytes, item.detail, item.created_at},
              nullptr);
  exec("DELETE FROM history WHERE id NOT IN (SELECT id FROM history ORDER BY id DESC LIMIT 5000)",
       nullptr);
}

std::vector<HistoryItem> Store::history(const std::string& kind, int limit) const {
  std::vector<HistoryItem> out;
  const int safe_limit = std::max(1, std::min(limit, 500));
  const auto table = kind.empty()
                         ? query("SELECT id,user_id,kind,summary,status,http_status,latency_ms,"
                                 "size_bytes,detail,created_at FROM history ORDER BY id DESC LIMIT ?",
                                 {safe_limit})
                         : query("SELECT id,user_id,kind,summary,status,http_status,latency_ms,"
                                 "size_bytes,detail,created_at FROM history WHERE kind = ? "
                                 "ORDER BY id DESC LIMIT ?",
                                 {kind, safe_limit});
  if (!table) return out;
  for (const auto& row : table->rows()) {
    HistoryItem item;
    item.id = row["id"].to_int64();
    item.user_id = row["user_id"].to_int64();
    item.kind = row["kind"].to_text();
    item.summary = row["summary"].to_text();
    item.status = row["status"].to_text();
    item.http_status = static_cast<int>(row["http_status"].to_int64());
    item.latency_ms = row["latency_ms"].to_int64();
    item.size_bytes = row["size_bytes"].to_int64();
    item.detail = row["detail"].to_text();
    item.created_at = row["created_at"].to_text();
    out.push_back(std::move(item));
  }
  return out;
}

std::int64_t Store::history_count(const std::string& kind, std::int64_t since) const {
  (void)since;
  const auto table = kind.empty()
                         ? query("SELECT COUNT(*) AS c FROM history WHERE id > 0")
                         : query("SELECT COUNT(*) AS c FROM history WHERE kind = ?", {kind});
  return table && !table->empty() ? table->value("c").to_int64() : 0;
}

bool Store::clear_history(const std::string& kind, std::string* error) {
  if (kind.empty()) return clear_all_history(error);
  return exec("DELETE FROM history WHERE kind = ?", {kind}, error);
}

bool Store::clear_all_history(std::string* error) {
  return exec("DELETE FROM history", error);
}

std::vector<Socks5ServerConfig> Store::socks5_servers() const {
  std::vector<Socks5ServerConfig> out;
  const nlohmann::json parsed =
      nlohmann::json::parse(setting("socks5.servers", "[]"), nullptr, false);
  if (!parsed.is_array()) return out;
  for (const auto& item : parsed) {
    if (!item.is_object()) continue;
    Socks5ServerConfig server;
    server.id = item.value("id", std::string());
    server.name = item.value("name", std::string());
    server.host = item.value("host", std::string("127.0.0.1"));
    server.port = item.value("port", 0);
    server.auto_start = item.value("autoStart", false);
    server.auth_required = item.value("authRequired", false);
    server.username = item.value("username", std::string());
    server.password_hash = item.value("passwordHash", std::string());
    server.created_at = item.value("createdAt", std::string());
    server.updated_at = item.value("updatedAt", std::string());
    if (server.id.empty() || server.host.empty() || server.port <= 0 ||
        server.port > 65535 || (server.auth_required && server.password_hash.empty())) {
      continue;
    }
    out.push_back(std::move(server));
  }
  return out;
}

std::optional<Socks5ServerConfig> Store::socks5_server(const std::string& id) const {
  for (const auto& server : socks5_servers()) {
    if (server.id == id) return server;
  }
  return std::nullopt;
}

bool Store::save_socks5_server(const Socks5ServerConfig& entry, std::string* error) {
  if (entry.id.empty() || entry.host.empty() || entry.port <= 0 || entry.port > 65535 ||
      (entry.auth_required && entry.password_hash.empty())) {
    if (error) *error = "invalid SOCKS5 server";
    return false;
  }
  auto list = socks5_servers();
  bool found = false;
  for (auto& item : list) {
    if (item.id != entry.id) continue;
    item = entry;
    if (item.created_at.empty()) item.created_at = now_iso8601();
    item.updated_at = now_iso8601();
    found = true;
    break;
  }
  if (!found) {
    Socks5ServerConfig copy = entry;
    if (copy.created_at.empty()) copy.created_at = now_iso8601();
    copy.updated_at = now_iso8601();
    list.push_back(std::move(copy));
  }
  nlohmann::json out = nlohmann::json::array();
  for (const auto& item : list) {
    out.push_back({{"id", item.id},
                   {"name", item.name},
                   {"host", item.host},
                   {"port", item.port},
                   {"autoStart", item.auto_start},
                   {"authRequired", item.auth_required},
                   {"username", item.username},
                   {"passwordHash", item.password_hash},
                   {"createdAt", item.created_at},
                   {"updatedAt", item.updated_at}});
  }
  return set_setting("socks5.servers", out.dump(), error);
}

bool Store::delete_socks5_server(const std::string& id, std::string* error) {
  auto list = socks5_servers();
  const auto before = list.size();
  list.erase(std::remove_if(list.begin(), list.end(),
                            [&](const Socks5ServerConfig& item) { return item.id == id; }),
             list.end());
  if (list.size() == before) {
    if (error) *error = "SOCKS5 server not found";
    return false;
  }
  nlohmann::json out = nlohmann::json::array();
  for (const auto& item : list) {
    out.push_back({{"id", item.id},
                   {"name", item.name},
                   {"host", item.host},
                   {"port", item.port},
                   {"autoStart", item.auto_start},
                   {"authRequired", item.auth_required},
                   {"username", item.username},
                   {"passwordHash", item.password_hash},
                   {"createdAt", item.created_at},
                   {"updatedAt", item.updated_at}});
  }
  return set_setting("socks5.servers", out.dump(), error);
}

std::vector<ProxyConfig> Store::proxies() const {
  std::vector<ProxyConfig> out;
  const auto table = query("SELECT id,name,listen_host,listen_port,target,auto_start,tls,"
                           "cert_path,key_path,rules_json,upstreams_json,retries,"
                           "connect_timeout_ms,request_timeout_ms,workers,"
                           "breaker_threshold,breaker_cooldown_ms,isolated,"
                           "created_at,updated_at FROM proxies ORDER BY name");
  if (!table) return out;
  for (const auto& row : table->rows()) out.push_back(row_to_proxy(row));
  return out;
}

std::optional<ProxyConfig> Store::proxy(const std::string& id) const {
  const auto table = query("SELECT id,name,listen_host,listen_port,target,auto_start,tls,"
                           "cert_path,key_path,rules_json,upstreams_json,retries,"
                           "connect_timeout_ms,request_timeout_ms,workers,"
                           "breaker_threshold,breaker_cooldown_ms,isolated,"
                           "created_at,updated_at FROM proxies WHERE id = ?",
                           {id});
  if (!table || table->empty()) return std::nullopt;
  return row_to_proxy(table->rows().front());
}

bool Store::save_proxy(const ProxyConfig& proxy, std::string* error) {
  const std::string now = now_iso8601();
  return exec(
      "INSERT INTO proxies(id,name,listen_host,listen_port,target,auto_start,tls,cert_path,key_path,"
      "rules_json,upstreams_json,retries,connect_timeout_ms,request_timeout_ms,workers,"
      "breaker_threshold,breaker_cooldown_ms,isolated,created_at,updated_at) "
      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
      "ON CONFLICT(id) DO UPDATE SET name=excluded.name, listen_host=excluded.listen_host, "
      "listen_port=excluded.listen_port, target=excluded.target, auto_start=excluded.auto_start, "
      "tls=excluded.tls, cert_path=excluded.cert_path, key_path=excluded.key_path, "
      "rules_json=excluded.rules_json, upstreams_json=excluded.upstreams_json, "
      "retries=excluded.retries, connect_timeout_ms=excluded.connect_timeout_ms, "
      "request_timeout_ms=excluded.request_timeout_ms, workers=excluded.workers, "
      "breaker_threshold=excluded.breaker_threshold, "
      "breaker_cooldown_ms=excluded.breaker_cooldown_ms, "
      "isolated=excluded.isolated, updated_at=excluded.updated_at",
      {proxy.id, proxy.name, proxy.listen_host, proxy.listen_port, proxy.target,
       proxy.auto_start ? 1 : 0, proxy.tls ? 1 : 0, proxy.cert_path, proxy.key_path,
       proxy_rules_to_json(proxy.rules).dump(),
       proxy_upstreams_to_json(proxy.upstreams).dump(),
       std::max(0, std::min(proxy.retries, 3)),
       std::max(100, std::min(proxy.connect_timeout_ms, 120000)),
       std::max(100, std::min(proxy.request_timeout_ms, 300000)),
       std::max(1, std::min(proxy.workers, 16)),
       std::max(0, std::min(proxy.breaker_threshold, 100)),
       std::max(1000, std::min(proxy.breaker_cooldown_ms, 300000)),
       proxy.isolated ? 1 : 0,
       proxy.created_at.empty() ? now : proxy.created_at, now},
      error);
}

bool Store::delete_proxy(const std::string& id, std::string* error) {
  return exec("DELETE FROM proxies WHERE id = ?", {id}, error);
}

std::vector<ListenerConfig> Store::listeners() const {
  std::vector<ListenerConfig> out;
  const nlohmann::json parsed =
      nlohmann::json::parse(setting("transport.listeners", "[]"), nullptr, false);
  if (!parsed.is_array()) return out;
  for (const auto& item : parsed) {
    if (!item.is_object()) continue;
    ListenerConfig listener;
    listener.id = item.value("id", std::string());
    listener.transport = item.value("transport", std::string("tcp"));
    listener.name = item.value("name", std::string());
    listener.host = item.value("host", std::string("127.0.0.1"));
    listener.port = item.value("port", 0);
    listener.auto_start = item.value("autoStart", false);
    listener.created_at = item.value("createdAt", std::string());
    listener.updated_at = item.value("updatedAt", std::string());
    if (listener.id.empty() || (listener.transport != "tcp" && listener.transport != "udp") ||
        listener.port <= 0 || listener.port > 65535) {
      continue;
    }
    out.push_back(std::move(listener));
  }
  return out;
}

std::optional<ListenerConfig> Store::listener(const std::string& id) const {
  for (const auto& listener : listeners()) {
    if (listener.id == id) return listener;
  }
  return std::nullopt;
}

bool Store::save_listener(const ListenerConfig& listener, std::string* error) {
  if (listener.id.empty() || (listener.transport != "tcp" && listener.transport != "udp") ||
      listener.host.empty() || listener.port <= 0 || listener.port > 65535) {
    if (error) *error = "invalid listener";
    return false;
  }
  auto list = listeners();
  bool found = false;
  for (auto& item : list) {
    if (item.id == listener.id) {
      item = listener;
      if (item.created_at.empty()) item.created_at = now_iso8601();
      item.updated_at = now_iso8601();
      found = true;
      break;
    }
  }
  if (!found) {
    ListenerConfig copy = listener;
    if (copy.created_at.empty()) copy.created_at = now_iso8601();
    copy.updated_at = now_iso8601();
    list.push_back(std::move(copy));
  }
  nlohmann::json out = nlohmann::json::array();
  for (const auto& item : list) {
    out.push_back({{"id", item.id},
                   {"transport", item.transport},
                   {"name", item.name},
                   {"host", item.host},
                   {"port", item.port},
                   {"autoStart", item.auto_start},
                   {"createdAt", item.created_at},
                   {"updatedAt", item.updated_at}});
  }
  return set_setting("transport.listeners", out.dump(), error);
}

bool Store::delete_listener(const std::string& id, std::string* error) {
  auto list = listeners();
  const auto before = list.size();
  list.erase(std::remove_if(list.begin(), list.end(),
                            [&](const ListenerConfig& item) { return item.id == id; }),
             list.end());
  if (list.size() == before) {
    if (error) *error = "listener not found";
    return false;
  }
  nlohmann::json out = nlohmann::json::array();
  for (const auto& item : list) {
    out.push_back({{"id", item.id},
                   {"transport", item.transport},
                   {"name", item.name},
                   {"host", item.host},
                   {"port", item.port},
                   {"autoStart", item.auto_start},
                   {"createdAt", item.created_at},
                   {"updatedAt", item.updated_at}});
  }
  return set_setting("transport.listeners", out.dump(), error);
}


static TerminalCommand row_to_terminal_command(const uvcpp::uvcpp_db_row& row) {
  TerminalCommand command;
  command.id = row["id"].to_int64();
  command.command = row["command"].to_text();
  command.name_en = row["name_en"].to_text();
  command.name_zh = row["name_zh"].to_text();
  command.description_en = row["description_en"].to_text();
  command.description_zh = row["description_zh"].to_text();
  command.category = row["category"].to_text();
  command.platform = row["platform"].to_text();
  command.tags = row["tags"].to_text();
  command.quick = row["quick"].to_bool();
  command.builtin = row["builtin"].to_bool();
  command.owner_id = row["owner_id"].to_int64();
  command.created_at = row["created_at"].to_text();
  command.updated_at = row["updated_at"].to_text();
  return command;
}

std::vector<TerminalCommand> Store::terminal_commands(
    std::int64_t owner_id, const std::string& platform,
    const std::string& category, const std::string& search, bool all) const {
  std::vector<TerminalCommand> out;
  std::string sql =
      "SELECT id,command,name_en,name_zh,description_en,description_zh,category,"
      "platform,tags,quick,builtin,owner_id,created_at,updated_at "
      "FROM terminal_commands WHERE ";
  uvcpp::uvcpp_db_params params;
  if (all) {
    sql += "1=1";
  } else {
    sql += "(builtin = 1 OR owner_id = ?)";
    params.add(owner_id);
  }
  if (!platform.empty()) {
    sql += " AND (platform = ? OR platform = 'both')";
    params.add(platform);
  }
  if (!category.empty()) {
    sql += " AND category = ?";
    params.add(category);
  }
  if (!search.empty()) {
    sql +=
        " AND (command LIKE ? OR name_en LIKE ? OR name_zh LIKE ? OR "
        "description_en LIKE ? OR description_zh LIKE ? OR tags LIKE ?)";
    const std::string like = "%" + search + "%";
    for (int i = 0; i < 6; ++i) params.add(like);
  }
  sql += " ORDER BY builtin DESC, category, name_en LIMIT 500";
  const auto table = query(sql, params);
  if (!table) return out;
  for (const auto& row : table->rows()) out.push_back(row_to_terminal_command(row));
  return out;
}

std::optional<TerminalCommand> Store::terminal_command(std::int64_t id) const {
  const auto table = query(
      "SELECT id,command,name_en,name_zh,description_en,description_zh,category,"
      "platform,tags,quick,builtin,owner_id,created_at,updated_at "
      "FROM terminal_commands WHERE id = ?",
      {id});
  if (!table || table->empty()) return std::nullopt;
  return row_to_terminal_command(table->rows().front());
}

bool Store::add_terminal_command(TerminalCommand& command, std::string* error) {
  const std::string now = now_iso8601();
  if (!exec("INSERT INTO terminal_commands(command,name_en,name_zh,description_en,"
            "description_zh,category,platform,tags,quick,builtin,owner_id,created_at,"
            "updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
            {command.command, command.name_en, command.name_zh, command.description_en,
             command.description_zh, command.category, command.platform, command.tags,
             command.quick ? 1 : 0, 0, command.owner_id, now, now},
            error)) {
    return false;
  }
  command.id = db_.last_insert_id().to_int64();
  command.builtin = false;
  command.created_at = now;
  command.updated_at = now;
  return true;
}

bool Store::update_terminal_command(const TerminalCommand& command,
                                    std::int64_t owner_id, bool admin,
                                    std::string* error) {
  const auto table = query(
      "SELECT builtin,owner_id FROM terminal_commands WHERE id = ?", {command.id});
  if (!table || table->empty()) {
    if (error) *error = "command not found";
    return false;
  }
  const bool builtin = table->rows().front()["builtin"].to_bool();
  const std::int64_t current_owner = table->rows().front()["owner_id"].to_int64();
  if (!admin && (builtin || current_owner != owner_id)) {
    if (error) *error = "you can only edit your own custom commands";
    return false;
  }
  return exec("UPDATE terminal_commands SET command=?, name_en=?, name_zh=?, "
              "description_en=?, description_zh=?, category=?, platform=?, tags=?, "
              "quick=?, updated_at=? WHERE id=?",
              {command.command, command.name_en, command.name_zh, command.description_en,
               command.description_zh, command.category, command.platform, command.tags,
               command.quick ? 1 : 0, now_iso8601(), command.id},
              error);
}

bool Store::delete_terminal_command(std::int64_t id, std::int64_t owner_id,
                                    bool admin, std::string* error) {
  const auto table = query(
      "SELECT builtin,owner_id FROM terminal_commands WHERE id = ?", {id});
  if (!table || table->empty()) {
    if (error) *error = "command not found";
    return false;
  }
  const bool builtin = table->rows().front()["builtin"].to_bool();
  const std::int64_t current_owner = table->rows().front()["owner_id"].to_int64();
  if (!admin && (builtin || current_owner != owner_id)) {
    if (error) *error = "you can only delete your own custom commands";
    return false;
  }
  return exec("DELETE FROM terminal_commands WHERE id = ?", {id}, error);
}

bool Store::restore_terminal_commands(std::string* error) {
  const std::string now = now_iso8601();
  for (const auto& seed : terminal_command_seeds()) {
    const auto existing = query(
        "SELECT id FROM terminal_commands WHERE command = ? AND platform = ? LIMIT 1",
        {seed.command, seed.platform});
    if (existing && !existing->empty()) continue;
    if (!exec("INSERT INTO terminal_commands(command,name_en,name_zh,description_en,"
              "description_zh,category,platform,tags,quick,builtin,owner_id,created_at,"
              "updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
              {seed.command, seed.name_en, seed.name_zh, seed.description_en,
               seed.description_zh, seed.category, seed.platform, seed.tags,
               seed.quick ? 1 : 0, 1, 0, now, now},
              error)) {
      return false;
    }
  }
  return true;
}

std::int64_t Store::user_count() const {
  const auto table = query("SELECT COUNT(*) AS c FROM users");
  return table && !table->empty() ? table->value("c").to_int64() : 0;
}

User Store::row_to_user(const uvcpp::uvcpp_db_row& row) {
  User user;
  user.id = row["id"].to_int64();
  user.username = row["username"].to_text();
  user.role = row["role"].to_text();
  user.enabled = row["enabled"].to_bool();
  user.created_at = row["created_at"].to_text();
  user.last_login = row.has("last_login") ? row["last_login"].to_text() : std::string();
  return user;
}


nlohmann::json terminal_command_json(const TerminalCommand& command) {
  return {{"id", command.id},
          {"command", command.command},
          {"nameEn", command.name_en},
          {"nameZh", command.name_zh},
          {"descriptionEn", command.description_en},
          {"descriptionZh", command.description_zh},
          {"category", command.category},
          {"platform", command.platform},
          {"tags", command.tags},
          {"quick", command.quick},
          {"builtin", command.builtin},
          {"ownerId", command.owner_id},
          {"createdAt", command.created_at},
          {"updatedAt", command.updated_at}};
}

nlohmann::json proxy_rules_json(const std::vector<ProxyRule>& rules) {
  return proxy_rules_to_json(rules);
}

nlohmann::json proxy_upstreams_json(const std::vector<ProxyUpstream>& upstreams) {
  return proxy_upstreams_to_json(upstreams);
}

ProxyConfig Store::row_to_proxy(const uvcpp::uvcpp_db_row& row) {
  ProxyConfig p;
  p.id = row["id"].to_text();
  p.name = row["name"].to_text();
  p.listen_host = row["listen_host"].to_text();
  p.listen_port = static_cast<int>(row["listen_port"].to_int64());
  p.target = row["target"].to_text();
  p.auto_start = row["auto_start"].to_bool();
  p.tls = row["tls"].to_bool();
  p.cert_path = row["cert_path"].to_text();
  p.key_path = row["key_path"].to_text();
  p.rules = proxy_rules_from_json(row.has("rules_json") ? row["rules_json"].to_text()
                                                       : std::string("[]"));
  p.upstreams = proxy_upstreams_from_json(
      row.has("upstreams_json") ? row["upstreams_json"].to_text() : std::string("[]"));
  p.retries = row.has("retries") ? static_cast<int>(row["retries"].to_int64()) : 0;
  p.connect_timeout_ms =
      row.has("connect_timeout_ms")
          ? std::max(100, std::min(120000,
                                   static_cast<int>(row["connect_timeout_ms"].to_int64())))
          : 15000;
  p.request_timeout_ms =
      row.has("request_timeout_ms")
          ? std::max(100, std::min(300000,
                                   static_cast<int>(row["request_timeout_ms"].to_int64())))
          : 30000;
  p.workers = row.has("workers")
                  ? std::max(1, std::min(16, static_cast<int>(row["workers"].to_int64())))
                  : 4;
  p.breaker_threshold =
      row.has("breaker_threshold")
          ? std::max(0, std::min(100, static_cast<int>(row["breaker_threshold"].to_int64())))
          : 5;
  p.breaker_cooldown_ms =
      row.has("breaker_cooldown_ms")
          ? std::max(1000, std::min(300000,
                                    static_cast<int>(row["breaker_cooldown_ms"].to_int64())))
          : 30000;
  p.isolated = row.has("isolated") && row["isolated"].to_bool();
  p.created_at = row["created_at"].to_text();
  p.updated_at = row["updated_at"].to_text();
  return p;
}

const char* role_permission_summary(const std::string& role) {
  if (role == "admin") return "Full access, user management, settings and reset";
  if (role == "operator") return "Use debug modules, history and reverse proxies";
  return "Read-only dashboard and history";
}

bool role_valid(const std::string& role) {
  return role == "admin" || role == "operator" || role == "viewer";
}

bool role_can(const std::string& role, const std::string& permission) {
  if (role == "admin") return true;
  if (role == "viewer") {
    return permission == "dashboard" || permission == "history.read" || permission == "about";
  }
  if (role == "operator") {
    return permission != "admin.users" && permission != "admin.settings" &&
           permission != "admin.reset";
  }
  return false;
}

}  // namespace uvscope
