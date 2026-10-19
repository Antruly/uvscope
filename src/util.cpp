#include "util.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include <uv.h>
#include <openssl/rand.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#if defined(_WIN32)
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace uvscope {
namespace {

int default_port_for(const std::string& scheme) {
  if (scheme == "https" || scheme == "wss") return 443;
  if (scheme == "http" || scheme == "ws") return 80;
  return 80;
}

bool valid_host_char(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  return std::isalnum(u) || c == '.' || c == '-' || c == '_' || c == ':' || c == '[' || c == ']';
}

}  // namespace

ParsedUrl parse_url(const std::string& input, const std::string& default_scheme) {
  ParsedUrl out;
  std::string s = trim(input);
  if (s.empty()) {
    out.error = "URL is empty";
    return out;
  }

  std::string rest = s;
  const std::size_t scheme_pos = s.find("://");
  if (scheme_pos != std::string::npos) {
    out.scheme = lower(s.substr(0, scheme_pos));
    rest = s.substr(scheme_pos + 3);
  } else {
    if (s.rfind("//", 0) == 0) {
      rest = s.substr(2);
    }
    out.scheme = lower(default_scheme);
  }
  if (out.scheme.empty()) out.scheme = "http";
  if (!is_http_scheme(out.scheme)) {
    out.error = "unsupported scheme: " + out.scheme;
    return out;
  }
  out.port = default_port_for(out.scheme);

  const std::size_t authority_end = rest.find_first_of("/?#");
  std::string authority = rest.substr(0, authority_end);
  std::string tail = authority_end == std::string::npos ? std::string() : rest.substr(authority_end);
  if (authority.empty()) {
    out.error = "URL has no host";
    return out;
  }
  const std::size_t at = authority.rfind('@');
  if (at != std::string::npos) {
    out.userinfo = authority.substr(0, at);
    authority = authority.substr(at + 1);
  }

  std::string port_text;
  if (!authority.empty() && authority[0] == '[') {
    const std::size_t close = authority.find(']');
    if (close == std::string::npos) {
      out.error = "invalid IPv6 host";
      return out;
    }
    out.host = authority.substr(1, close - 1);
    if (close + 1 < authority.size() && authority[close + 1] == ':') {
      port_text = authority.substr(close + 2);
    }
  } else {
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(':') == colon) {
      out.host = authority.substr(0, colon);
      port_text = authority.substr(colon + 1);
    } else {
      out.host = authority;
    }
  }
  if (out.host.empty() || !std::all_of(out.host.begin(), out.host.end(), valid_host_char)) {
    out.error = "invalid host";
    return out;
  }
  if (!port_text.empty()) {
    char* end = nullptr;
    const long port = std::strtol(port_text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || port <= 0 || port > 65535) {
      out.error = "invalid port";
      return out;
    }
    out.port = static_cast<int>(port);
  }

  const std::size_t hash = tail.find('#');
  if (hash != std::string::npos) tail.erase(hash);
  const std::size_t q = tail.find('?');
  if (q != std::string::npos) {
    out.path = tail.substr(0, q);
    out.query = tail.substr(q + 1);
  } else {
    out.path = tail;
  }
  if (out.path.empty()) out.path = "/";
  if (out.path[0] != '/') out.path.insert(out.path.begin(), '/');
  out.valid = true;
  return out;
}

std::string url_path_with_query(const ParsedUrl& url) {
  std::string path = url.path.empty() ? "/" : url.path;
  if (!url.query.empty()) path += "?" + url.query;
  return path;
}

bool is_http_scheme(const std::string& scheme) {
  return scheme == "http" || scheme == "https";
}

std::string trim(std::string value) {
  const auto not_space = [](unsigned char c) { return !std::isspace(c); };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
  return value;
}

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string join_path(const std::string& base, const std::string& suffix) {
  if (base.empty()) return suffix.empty() ? "/" : suffix;
  if (suffix.empty() || suffix == "/") return base;
  if (base.back() == '/' && suffix.front() == '/') return base + suffix.substr(1);
  if (base.back() != '/' && suffix.front() != '/') return base + "/" + suffix;
  return base + suffix;
}

std::string url_decode(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2 < value.size() && std::isxdigit(static_cast<unsigned char>(value[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(value[i + 2]))) {
      const auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return c - 'A' + 10;
      };
      out.push_back(static_cast<char>((hexval(value[i + 1]) << 4) | hexval(value[i + 2])));
      i += 2;
    } else if (value[i] == '+') {
      out.push_back(' ');
    } else {
      out.push_back(value[i]);
    }
  }
  return out;
}

std::string random_hex(std::size_t bytes) {
  std::vector<unsigned char> data(bytes);
  if (RAND_bytes(data.data(), static_cast<int>(data.size())) != 1) {
    std::random_device rd;
    for (auto& b : data) b = static_cast<unsigned char>(rd());
  }
  static const char* hex = "0123456789abcdef";
  std::string out;
  out.reserve(bytes * 2);
  for (unsigned char b : data) {
    out.push_back(hex[b >> 4]);
    out.push_back(hex[b & 0x0F]);
  }
  return out;
}

std::string base64_encode(const std::string& input) {
  static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((input.size() + 2) / 3) * 4);
  const auto* data = reinterpret_cast<const unsigned char*>(input.data());
  std::size_t i = 0;
  while (i + 3 <= input.size()) {
    const std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16) |
                            (static_cast<std::uint32_t>(data[i + 1]) << 8) |
                            static_cast<std::uint32_t>(data[i + 2]);
    out.push_back(table[(n >> 18) & 63]);
    out.push_back(table[(n >> 12) & 63]);
    out.push_back(table[(n >> 6) & 63]);
    out.push_back(table[n & 63]);
    i += 3;
  }
  if (i < input.size()) {
    const std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16 |
                            (i + 1 < input.size() ? static_cast<std::uint32_t>(data[i + 1]) << 8 : 0);
    out.push_back(table[(n >> 18) & 63]);
    out.push_back(table[(n >> 12) & 63]);
    out.push_back(i + 1 < input.size() ? table[(n >> 6) & 63] : '=');
    out.push_back('=');
  }
  return out;
}

std::optional<std::string> base64_decode(const std::string& input) {
  static const std::string table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::array<int, 256> rev{};
  rev.fill(-1);
  for (std::size_t i = 0; i < table.size(); ++i) rev[static_cast<unsigned char>(table[i])] = static_cast<int>(i);
  std::string out;
  int val = 0;
  int bits = -8;
  for (unsigned char c : input) {
    if (c == '=') break;
    if (std::isspace(c)) continue;
    const int d = rev[c];
    if (d < 0) return std::nullopt;
    val = (val << 6) + d;
    bits += 6;
    if (bits >= 0) {
      out.push_back(static_cast<char>((val >> bits) & 0xFF));
      bits -= 8;
    }
  }
  return out;
}

std::string hex_encode(const std::string& bytes, bool spaced) {
  static const char* hex = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * (spaced ? 3 : 2));
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    const unsigned char b = static_cast<unsigned char>(bytes[i]);
    if (spaced && i) out.push_back(' ');
    out.push_back(hex[b >> 4]);
    out.push_back(hex[b & 0x0F]);
  }
  return out;
}

std::optional<std::string> hex_decode(std::string text) {
  text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) {
    return std::isspace(c) || c == ':' || c == '-';
  }), text.end());
  if (text.size() % 2 != 0) return std::nullopt;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string out;
  out.reserve(text.size() / 2);
  for (std::size_t i = 0; i < text.size(); i += 2) {
    const int hi = nibble(text[i]);
    const int lo = nibble(text[i + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    out.push_back(static_cast<char>((hi << 4) | lo));
  }
  return out;
}

std::string now_iso8601() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  std::ostringstream out;
  out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

std::int64_t now_unix() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string format_rfc2822(std::int64_t unix_time) {
  const std::time_t t = static_cast<std::time_t>(unix_time);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  std::ostringstream out;
  out << std::put_time(&tm, "%a, %d %b %Y %H:%M:%S GMT");
  return out.str();
}

std::filesystem::path default_data_dir() {
  const std::string explicit_dir = env_string("UVSCOPE_DATA_DIR");
  if (!explicit_dir.empty()) return std::filesystem::path(explicit_dir);
#if defined(_WIN32)
  const std::string appdata = env_string("APPDATA");
  if (!appdata.empty()) return std::filesystem::path(appdata) / "uvscope";
  return std::filesystem::path("uvscope-data");
#elif defined(__APPLE__)
  const std::string home = env_string("HOME");
  if (!home.empty()) return std::filesystem::path(home) / "Library" / "Application Support" / "uvscope";
  return std::filesystem::path("uvscope-data");
#else
  const std::string xdg = env_string("XDG_DATA_HOME");
  if (!xdg.empty()) return std::filesystem::path(xdg) / "uvscope";
  const std::string home = env_string("HOME");
  if (!home.empty()) return std::filesystem::path(home) / ".local" / "share" / "uvscope";
  return std::filesystem::path("uvscope-data");
#endif
}

std::filesystem::path executable_dir(const char* argv0) {
#if defined(_WIN32)
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (n == 0) break;
    if (n < buffer.size()) {
      buffer.resize(n);
      return std::filesystem::path(buffer).parent_path();
    }
    buffer.resize(buffer.size() * 2);
  }
#elif defined(__linux__)
  std::array<char, 4096> path{};
  const ssize_t n = readlink("/proc/self/exe", path.data(), path.size() - 1);
  if (n > 0) {
    path[static_cast<std::size_t>(n)] = '\0';
    return std::filesystem::path(path.data()).parent_path();
  }
#elif defined(__APPLE__)
  std::array<char, 4096> path{};
  uint32_t size = static_cast<uint32_t>(path.size());
  if (_NSGetExecutablePath(path.data(), &size) == 0) {
    return std::filesystem::weakly_canonical(std::filesystem::path(path.data())).parent_path();
  }
#endif
  if (argv0 != nullptr && *argv0 != '\0') {
    std::error_code ec;
    const auto p = std::filesystem::absolute(std::filesystem::path(argv0), ec);
    if (!ec) return p.parent_path();
  }
  return std::filesystem::current_path();
}

std::filesystem::path default_web_root(const std::filesystem::path& exe_dir) {
  const std::string env_root = env_string("UVSCOPE_WEB_ROOT");
  const std::vector<std::filesystem::path> candidates = {
      env_root.empty() ? std::filesystem::path() : std::filesystem::path(env_root),
      exe_dir / "web",
      exe_dir / ".." / "share" / "uvscope" / "web",
      exe_dir / ".." / "share" / "uvscope",
      std::filesystem::current_path() / "web" / "dist",
      std::filesystem::current_path() / "web"};
  for (const auto& candidate : candidates) {
    if (candidate.empty()) continue;
    std::error_code ec;
    if (std::filesystem::exists(candidate / "index.html", ec)) {
      return std::filesystem::weakly_canonical(candidate, ec);
    }
  }
  return exe_dir / "web";
}

std::string read_file(const std::filesystem::path& path, std::size_t max_bytes) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size < 0 || static_cast<std::size_t>(size) > max_bytes) return {};
  in.seekg(0, std::ios::beg);
  std::string data(static_cast<std::size_t>(size), '\0');
  in.read(data.data(), static_cast<std::streamsize>(data.size()));
  if (!in && !in.eof()) return {};
  return data;
}

bool write_file(const std::filesystem::path& path, const std::string& bytes) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return out.good();
}

bool ensure_directory(const std::filesystem::path& path, std::string* error) {
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    if (std::filesystem::is_directory(path, ec)) return true;
    if (error != nullptr) *error = "path exists but is not a directory";
    return false;
  }
  std::filesystem::create_directories(path, ec);
  if (ec) {
    if (error != nullptr) *error = ec.message();
    return false;
  }
  return true;
}

std::string env_string(const char* name, const std::string& fallback) {
  if (name == nullptr) return fallback;
  const char* value = std::getenv(name);
  if (value == nullptr) return fallback;
  return std::string(value);
}

bool constant_time_equal(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  }
  return diff == 0;
}

std::string make_id() {
  return random_hex(12);
}

}  // namespace uvscope

namespace uvscope {

std::string uv_error_string(int code) {
  if (code == 0) return {};
  switch (code) {
    case UV_EOF: return "connection closed by peer";
    case UV_ETIMEDOUT: return "operation timed out";
    case UV_ECANCELED: return "operation cancelled";
    case UV_ECONNREFUSED: return "connection refused";
    case UV_ECONNRESET: return "connection reset";
    case UV_ENOTCONN: return "not connected";
    case UV_EINVAL: return "invalid argument";
    case UV_ENOMEM: return "out of memory";
    case UV_EACCES: return "permission denied";
    case UV_ENOENT: return "not found";
    case UV_EAI_NONAME: return "host not found";
    case UV_EAI_AGAIN: return "temporary DNS failure";
    case UV_EAI_FAIL: return "DNS lookup failed";
    case UV_EALREADY: return "operation already in progress";
    case UV_EBUSY: return "resource busy";
    case UV_EPIPE: return "broken pipe";
    default: return "libuv error " + std::to_string(code);
  }
}

std::string libuv_version_string() {
  return std::to_string(UV_VERSION_MAJOR) + "." + std::to_string(UV_VERSION_MINOR) + "." +
         std::to_string(UV_VERSION_PATCH);
}

std::string find_ca_bundle(const std::filesystem::path& exe_dir) {
  const std::string explicit_file = env_string("UVSCOPE_CA_FILE");
  if (!explicit_file.empty()) return explicit_file;
  const std::string ssl_cert_file = env_string("SSL_CERT_FILE");
  if (!ssl_cert_file.empty()) return ssl_cert_file;
  const std::vector<std::filesystem::path> candidates = {
      exe_dir / "cacert.pem",
      exe_dir / ".." / "share" / "uvscope" / "cacert.pem",
#if defined(_WIN32)
      std::filesystem::path("C:/ProgramData/uvscope/cacert.pem"),
#elif defined(__APPLE__)
      std::filesystem::path("/etc/ssl/cert.pem"),
      std::filesystem::path("/usr/local/etc/openssl@3/cert.pem"),
#else
      std::filesystem::path("/etc/ssl/certs/ca-certificates.crt"),
      std::filesystem::path("/etc/pki/tls/certs/ca-bundle.crt"),
      std::filesystem::path("/etc/ssl/ca-bundle.pem"),
#endif
  };
  std::error_code ec;
  for (const auto& candidate : candidates) {
    if (std::filesystem::is_regular_file(candidate, ec)) return candidate.string();
  }
  return {};
}

int resident_memory_kb() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS counters{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == 0) return 0;
  return static_cast<int>(counters.WorkingSetSize / 1024);
#else
  struct rusage usage {};
  if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#if defined(__APPLE__)
  return static_cast<int>(usage.ru_maxrss / 1024);
#else
  return static_cast<int>(usage.ru_maxrss);
#endif
#endif
}

}  // namespace uvscope
