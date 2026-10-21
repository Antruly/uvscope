#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <db/uvcpp_db.h>
#include <nlohmann/json.hpp>

namespace uvscope {

struct User {
  std::int64_t id = 0;
  std::string username;
  std::string role = "viewer";
  bool enabled = true;
  std::string created_at;
  std::string last_login;
};

struct Session {
  std::string token;
  User user;
  std::int64_t expires_at = 0;
};

struct HistoryItem {
  std::int64_t id = 0;
  std::int64_t user_id = 0;
  std::string kind;
  std::string summary;
  std::string status;
  int http_status = 0;
  std::int64_t latency_ms = 0;
  std::int64_t size_bytes = 0;
  std::string detail;
  std::string created_at;
};

struct ListenerConfig {
  std::string id;
  std::string transport = "tcp";
  std::string name;
  std::string host = "127.0.0.1";
  int port = 0;
  bool auto_start = false;
  std::string created_at;
  std::string updated_at;
};

struct TerminalCommand {
  std::int64_t id = 0;
  std::string command;
  std::string name_en;
  std::string name_zh;
  std::string description_en;
  std::string description_zh;
  std::string category;
  std::string platform;  // linux | windows | both
  std::string tags;
  bool quick = false;
  bool builtin = false;
  std::int64_t owner_id = 0;
  std::string created_at;
  std::string updated_at;
};

struct Socks5ServerConfig {
  std::string id;
  std::string name;
  std::string host = "127.0.0.1";
  int port = 0;
  bool auto_start = false;
  bool auth_required = false;
  std::string username;
  // PBKDF2 hash produced by password_hash(); the plaintext password is never
  // persisted or returned by the API.
  std::string password_hash;
  std::string created_at;
  std::string updated_at;
};

struct ProxyRule {
  std::string id;
  std::string name;
  bool enabled = true;
  std::string match_type = "prefix";  // exact | prefix | regex
  std::string pattern;                // empty = any path (other conditions decide)
  // Optional conditions, combined with AND. Empty means "no condition".
  std::string method;       // comma-separated list, e.g. "GET,POST"
  std::string host;         // exact or *.suffix; port ignored unless present
  std::string header_name;  // header must exist when value is empty
  std::string header_value;
  std::string action = "redirect";    // redirect | rewrite
  int status = 302;
  std::string destination;
  bool preserve_query = true;
};

nlohmann::json proxy_rules_json(const std::vector<ProxyRule>& rules);
nlohmann::json terminal_command_json(const TerminalCommand& command);

struct ProxyConfig {
  std::string id;
  std::string name;
  std::string listen_host = "127.0.0.1";
  int listen_port = 0;
  std::string target;
  bool auto_start = false;
  bool tls = false;
  std::string cert_path;
  std::string key_path;
  std::vector<ProxyRule> rules;
  std::string created_at;
  std::string updated_at;
};

class Store {
 public:
  Store() = default;
  ~Store();

  bool open(const std::filesystem::path& database_path, std::string* error);
  void close();
  bool is_open() const { return db_.is_open(); }
  const std::string& last_error() const { return last_error_; }
  const std::filesystem::path& database_path() const { return database_path_; }

  bool migrate(std::string* error);
  bool initialized() const;
  bool initialize_admin(const std::string& username, const std::string& password, std::string* error);
  bool reset_database(std::string* error);

  std::optional<User> verify_login(const std::string& username, const std::string& password,
                                   std::string* error);
  std::optional<User> user_by_id(std::int64_t id) const;
  std::optional<User> user_by_username(const std::string& username) const;

  std::string create_session(std::int64_t user_id);
  std::optional<Session> session_by_token(const std::string& token) const;
  void delete_session(const std::string& token);
  void delete_sessions_for_user(std::int64_t user_id);
  void cleanup_sessions();

  std::vector<User> list_users() const;
  bool create_user(const std::string& username, const std::string& password,
                   const std::string& role, std::string* error);
  bool update_user(std::int64_t id, const std::optional<std::string>& role,
                   const std::optional<bool>& enabled,
                   const std::optional<std::string>& new_password, std::string* error);
  bool delete_user(std::int64_t id, std::string* error);
  bool change_password(std::int64_t user_id, const std::string& new_password,
                       std::string* error);
  bool set_last_login(std::int64_t user_id);

  std::string setting(const std::string& key, const std::string& fallback = std::string()) const;
  bool set_setting(const std::string& key, const std::string& value, std::string* error);
  std::map<std::string, bool> module_states() const;
  bool module_enabled(const std::string& module) const;
  bool set_module_enabled(const std::string& module, bool enabled, std::string* error);
  bool reset_module(const std::string& module, std::string* error);

  void add_history(HistoryItem item);
  std::vector<HistoryItem> history(const std::string& kind, int limit) const;
  std::int64_t history_count(const std::string& kind, std::int64_t since) const;
  bool clear_history(const std::string& kind, std::string* error);
  bool clear_all_history(std::string* error);

  std::vector<ListenerConfig> listeners() const;
  std::optional<ListenerConfig> listener(const std::string& id) const;
  bool save_listener(const ListenerConfig& listener, std::string* error);
  bool delete_listener(const std::string& id, std::string* error);

  std::vector<TerminalCommand> terminal_commands(
      std::int64_t owner_id, const std::string& platform,
      const std::string& category, const std::string& search, bool all) const;
  std::optional<TerminalCommand> terminal_command(std::int64_t id) const;
  bool add_terminal_command(TerminalCommand& command, std::string* error);
  bool update_terminal_command(const TerminalCommand& command, std::int64_t owner_id,
                               bool admin, std::string* error);
  bool delete_terminal_command(std::int64_t id, std::int64_t owner_id, bool admin,
                               std::string* error);
  bool restore_terminal_commands(std::string* error);

  std::vector<Socks5ServerConfig> socks5_servers() const;
  std::optional<Socks5ServerConfig> socks5_server(const std::string& id) const;
  bool save_socks5_server(const Socks5ServerConfig& server, std::string* error);
  bool delete_socks5_server(const std::string& id, std::string* error);

  std::vector<ProxyConfig> proxies() const;
  std::optional<ProxyConfig> proxy(const std::string& id) const;
  bool save_proxy(const ProxyConfig& proxy, std::string* error);
  bool delete_proxy(const std::string& id, std::string* error);

  std::int64_t user_count() const;

 private:
  bool exec(const std::string& sql, std::string* error);
  bool exec(const std::string& sql, const uvcpp::uvcpp_db_params& params, std::string* error);
  std::optional<uvcpp::uvcpp_db_table> query(const std::string& sql,
                                             const uvcpp::uvcpp_db_params& params = {},
                                             std::string* error = nullptr) const;
  void set_error(const std::string& message) const;
  static User row_to_user(const uvcpp::uvcpp_db_row& row);
  static ProxyConfig row_to_proxy(const uvcpp::uvcpp_db_row& row);

  mutable uvcpp::uvcpp_db_client db_;
  mutable std::string last_error_;
  std::filesystem::path database_path_;
};

const char* role_permission_summary(const std::string& role);
bool role_valid(const std::string& role);
bool role_can(const std::string& role, const std::string& permission);

}  // namespace uvscope
