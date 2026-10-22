#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "http_probe.h"

namespace uvscope {

struct DownloadSnapshot {
  std::string id;
  std::string state;  // running | ready | error | cancelled
  std::string error;
  std::string url;
  std::string contentType;
  std::string filename;
  std::string startedAt;
  std::string finishedAt;
  // Local staging path; never serialized into API responses.
  std::filesystem::path path;
  std::int64_t bytes = 0;
  std::int64_t totalBytes = -1;
  int statusCode = 0;
  // True when an errored job kept its partial file and can be resumed.
  bool resumable = false;
  int attempts = 0;
};

// Streams an upstream response to a temporary file on its own thread, then
// serves that file with the framework's ranged file transfer (backpressure and
// disconnect cancellation included). The HTTP probe stays the path for small
// bodies; this is for responses that would otherwise be clipped at 4 MiB.
class DownloadManager {
 public:
  explicit DownloadManager(std::filesystem::path data_dir);
  ~DownloadManager();

  DownloadManager(const DownloadManager&) = delete;
  DownloadManager& operator=(const DownloadManager&) = delete;

  struct Job;

  std::string start(const HttpProbeInput& input, std::int64_t owner_id,
                    std::string* error);
  // Continue an errored job from its retained partial file with a Range
  // request. The job id, owner and staged file stay the same.
  bool resume(const std::string& id, std::int64_t owner_id, bool is_admin,
              std::string* error);
  bool snapshot(const std::string& id, std::int64_t owner_id, bool is_admin,
                DownloadSnapshot* out) const;
  bool cancel(const std::string& id, std::int64_t owner_id, bool is_admin,
              std::string* error);
  // Public by token (the id is 32 random hex characters).
  std::optional<DownloadSnapshot> file(const std::string& id);

 private:
  std::shared_ptr<Job> find(const std::string& id) const;
  void prune_locked();

  std::filesystem::path dir_;
  mutable std::mutex mutex_;
  std::map<std::string, std::shared_ptr<Job>> jobs_;
};

nlohmann::json download_json(const DownloadSnapshot& snapshot);

}  // namespace uvscope
