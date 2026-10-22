#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace uvscope {

struct GenerateCertificateInput {
  std::string name;
  std::string common_name;
  std::vector<std::string> sans;
  int days = 365;
  std::string key_type = "rsa";  // rsa | ec
  int key_bits = 2048;           // RSA bits or EC curve (256/384)
};

struct ImportCertificateInput {
  std::string name;
  std::string cert_pem;
  std::string key_pem;
  std::string password;
};

// Managed server certificates stored under <data>/certs/<id>/ with
// cert.pem, key.pem and meta.json. Keys are 0600 on POSIX and never returned
// by the API; only their local paths are exposed.
class CertificateManager {
 public:
  explicit CertificateManager(std::filesystem::path directory);

  nlohmann::json list() const;
  nlohmann::json get(const std::string& id) const;
  nlohmann::json generate(const GenerateCertificateInput& input, std::string* error);
  nlohmann::json import(const ImportCertificateInput& input, std::string* error);
  // Rebuild a generated or CA-signed certificate in place, keeping its id and
  // paths so existing proxy configs stay valid (the listener reloads files on
  // restart). Imported certificates cannot be renewed.
  nlohmann::json renew(const std::string& id, int days, std::string* error);
  bool remove(const std::string& id, std::string* error);

  // Local certificate authority used to issue server certificates. The CA
  // lives next to the vault in <data>/ca/ and is created once by an admin.
  nlohmann::json ca_status() const;
  nlohmann::json create_ca(const std::string& common_name, int days, std::string* error);
  nlohmann::json issue(const GenerateCertificateInput& input, std::string* error);
  bool ca_exists() const;
  std::optional<std::string> ca_cert_path() const;

  std::optional<std::string> cert_path(const std::string& id) const;
  std::optional<std::string> key_path(const std::string& id) const;
  bool exists(const std::string& id) const;
  const std::filesystem::path& directory() const { return directory_; }

 private:
  std::filesystem::path ca_directory() const;

  std::filesystem::path directory_;
};

}  // namespace uvscope
