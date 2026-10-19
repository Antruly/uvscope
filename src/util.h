#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace uvscope {

struct ParsedUrl {
  std::string scheme;
  std::string host;
  int port = 80;
  std::string path = "/";
  std::string query;
  std::string userinfo;
  bool valid = false;
  std::string error;
};

ParsedUrl parse_url(const std::string& input, const std::string& default_scheme = "http");
std::string url_path_with_query(const ParsedUrl& url);
bool is_http_scheme(const std::string& scheme);

std::string trim(std::string value);
std::string lower(std::string value);
std::string join_path(const std::string& base, const std::string& suffix);
std::string url_decode(const std::string& value);

std::string random_hex(std::size_t bytes);
std::string base64_encode(const std::string& input);
std::optional<std::string> base64_decode(const std::string& input);
std::string hex_encode(const std::string& bytes, bool spaced = false);
std::optional<std::string> hex_decode(std::string text);

std::string now_iso8601();
std::int64_t now_unix();
std::string format_rfc2822(std::int64_t unix_time);

std::filesystem::path default_data_dir();
std::filesystem::path default_web_root(const std::filesystem::path& executable_dir);
std::filesystem::path executable_dir(const char* argv0);
std::string read_file(const std::filesystem::path& path, std::size_t max_bytes = 4 * 1024 * 1024);
bool write_file(const std::filesystem::path& path, const std::string& bytes);
bool ensure_directory(const std::filesystem::path& path, std::string* error = nullptr);

std::string env_string(const char* name, const std::string& fallback = std::string());
bool constant_time_equal(const std::string& a, const std::string& b);

std::string make_id();
std::string uv_error_string(int code);
std::string libuv_version_string();
int resident_memory_kb();
std::string find_ca_bundle(const std::filesystem::path& executable_dir);

}  // namespace uvscope
