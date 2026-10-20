#include "preview_store.h"

#include "util.h"

namespace uvscope {

std::optional<std::string> PreviewStore::put(const std::string& html,
                                             const std::string& base_url,
                                             const std::string& mode) {
  if (html.empty() || html.size() > kMaxDocumentBytes) return std::nullopt;
  std::lock_guard<std::mutex> lock(mutex_);
  prune_locked();
  while (entries_.size() >= kMaxEntries) {
    auto oldest = entries_.begin();
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
      if (it->second.expires < oldest->second.expires) oldest = it;
    }
    entries_.erase(oldest);
  }
  std::string id;
  do {
    id = random_hex(16);
  } while (entries_.count(id) != 0);
  entries_[id] = Entry{html, base_url, mode,
                       std::chrono::steady_clock::now() +
                           std::chrono::seconds(kTtlSeconds)};
  return id;
}

std::optional<PreviewStore::Document> PreviewStore::get(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex_);
  prune_locked();
  const auto it = entries_.find(id);
  if (it == entries_.end()) return std::nullopt;
  return Document{it->second.html, it->second.base_url, it->second.mode};
}

bool PreviewStore::remove(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.erase(id) != 0;
}

void PreviewStore::prune_locked() {
  const auto now = std::chrono::steady_clock::now();
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (it->second.expires <= now) {
      it = entries_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace uvscope
