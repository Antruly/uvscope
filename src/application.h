#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>
#include <webapp/uvcpp_web_app.h>

#include "auth.h"
#include "proxy_manager.h"
#include "socks5_manager.h"
#include "store.h"
#include "transport_manager.h"

namespace uvscope {

struct AppOptions {
  std::string host = "127.0.0.1";
  int port = 8787;
  std::filesystem::path data_dir;
  std::filesystem::path web_root;
  std::string tls_cert;
  std::string tls_key;
  bool self_check = false;
  bool open_browser = false;
};

struct AppMetrics {
  std::atomic<std::uint64_t> http_requests{0};
  std::atomic<std::uint64_t> http_errors{0};
  std::atomic<std::uint64_t> tcp_probes{0};
  std::atomic<std::uint64_t> udp_probes{0};
  std::atomic<std::uint64_t> tls_inspections{0};
  std::atomic<std::uint64_t> proxy_requests{0};
  std::atomic<std::uint64_t> proxied_bytes{0};
};

struct JobResult {
  int status = 200;
  nlohmann::json body = nlohmann::json::object();
};

class Application {
 public:
  explicit Application(AppOptions options);
  ~Application();

  int run();
  int self_check();

 private:
  void register_middleware();
  void register_public_routes();
  void register_auth_routes();
  void register_dashboard_routes();
  void register_debug_routes();
  void register_socks5_routes();
  void register_proxy_routes();
  void register_admin_routes();
  void register_update_routes();
  void install_static();

  void post_job(std::function<JobResult()> work, uvcpp::uvcpp_web_response* resp,
                uvcpp::uvcpp_web_next next, std::function<void(JobResult&)> after = {});
  void add_history(const Principal& principal, const std::string& kind,
                   const std::string& summary, const nlohmann::json& result);
  nlohmann::json user_json(const User& user) const;
  nlohmann::json github_get(const std::string& api_path, int* error_code,
                            std::string* error) const;
  nlohmann::json update_payload();
  nlohmann::json proxy_view() const;
  bool initialize();

  AppOptions options_;
  Store store_;
  Auth auth_;
  std::unique_ptr<ProxyManager> proxies_;
  std::unique_ptr<TransportManager> transport_;
  std::unique_ptr<Socks5Manager> socks5_;
  uvcpp::uvcpp_web_app app_;
  AppMetrics metrics_;
  std::chrono::steady_clock::time_point started_;
  std::string started_at_;
  std::string web_root_;

  mutable std::mutex update_mutex_;
  nlohmann::json update_cache_ = nlohmann::json::object();
  std::int64_t update_checked_at_ = 0;
  nlohmann::json repo_cache_ = nlohmann::json::object();
  std::thread update_thread_;
};

}  // namespace uvscope
