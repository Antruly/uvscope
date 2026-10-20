#pragma once

#include <chrono>
#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace uvscope {

// Short-lived, in-memory HTML documents served at /preview/:id for the
// "run scripts" render mode. Tokens are unguessable and expire; nothing is
// written to SQLite and the documents never carry the uvscope origin because
// the preview response forces `CSP: sandbox allow-scripts`.
class PreviewStore {
 public:
  static constexpr std::size_t kMaxDocumentBytes = 8 * 1024 * 1024;
  static constexpr std::size_t kMaxEntries = 8;
  static constexpr int kTtlSeconds = 120;

  struct Document {
    std::string html;
    std::string base_url;
    std::string mode;
  };

  // Returns a new token, or nullopt when the document is too large.
  std::optional<std::string> put(const std::string& html,
                                 const std::string& base_url,
                                 const std::string& mode);
  std::optional<Document> get(const std::string& id);
  bool remove(const std::string& id);

 private:
  struct Entry {
    std::string html;
    std::string base_url;
    std::string mode;
    std::chrono::steady_clock::time_point expires;
  };

  void prune_locked();

  mutable std::mutex mutex_;
  std::map<std::string, Entry> entries_;
};

}  // namespace uvscope
