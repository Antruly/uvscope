#include "certificate_manager.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <uv.h>

#include "certificate.h"
#include "util.h"

namespace uvscope {
namespace {

using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;
using X509Ptr = std::unique_ptr<X509, decltype(&X509_free)>;
using PkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

std::string bio_string(BIO* bio) {
  if (bio == nullptr) return {};
  char* data = nullptr;
  const long len = BIO_get_mem_data(bio, &data);
  if (len <= 0 || data == nullptr) return {};
  return std::string(data, static_cast<std::size_t>(len));
}

bool valid_id(const std::string& id) {
  if (id.size() != 24) return false;
  return std::all_of(id.begin(), id.end(), [](unsigned char c) {
    return std::isxdigit(c) != 0;
  });
}

bool is_ip_literal(const std::string& value) {
  struct sockaddr_in a4;
  struct sockaddr_in6 a6;
  return uv_ip4_addr(value.c_str(), 0, &a4) == 0 ||
         uv_ip6_addr(value.c_str(), 0, &a6) == 0;
}

std::string build_san(const std::vector<std::string>& sans) {
  std::string out;
  for (const auto& raw : sans) {
    const std::string value = trim(raw);
    if (value.empty()) continue;
    if (!out.empty()) out += ",";
    out += is_ip_literal(value) ? "IP:" + value : "DNS:" + value;
  }
  return out;
}

PkeyPtr generate_key(const std::string& type, int bits) {
  EVP_PKEY_CTX* ctx = nullptr;
  EVP_PKEY* key = nullptr;
  if (type == "ec") {
    ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    if (ctx == nullptr || EVP_PKEY_keygen_init(ctx) <= 0) {
      if (ctx) EVP_PKEY_CTX_free(ctx);
      return PkeyPtr(nullptr, &EVP_PKEY_free);
    }
    const int curve = bits == 384 ? NID_secp384r1 : NID_X9_62_prime256v1;
    if (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, curve) <= 0 ||
        EVP_PKEY_keygen(ctx, &key) <= 0) {
      EVP_PKEY_CTX_free(ctx);
      return PkeyPtr(nullptr, &EVP_PKEY_free);
    }
    EVP_PKEY_CTX_free(ctx);
    return PkeyPtr(key, &EVP_PKEY_free);
  }
  ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
  if (ctx == nullptr || EVP_PKEY_keygen_init(ctx) <= 0) {
    if (ctx) EVP_PKEY_CTX_free(ctx);
    return PkeyPtr(nullptr, &EVP_PKEY_free);
  }
  const int rsa_bits = (bits == 3072 || bits == 4096) ? bits : 2048;
  if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, rsa_bits) <= 0 ||
      EVP_PKEY_keygen(ctx, &key) <= 0) {
    EVP_PKEY_CTX_free(ctx);
    return PkeyPtr(nullptr, &EVP_PKEY_free);
  }
  EVP_PKEY_CTX_free(ctx);
  return PkeyPtr(key, &EVP_PKEY_free);
}

bool write_private_file(const std::filesystem::path& path, const std::string& data,
                        bool private_file) {
  if (!write_file(path, data)) return false;
  std::error_code ec;
  std::filesystem::permissions(
      path,
      private_file ? (std::filesystem::perms::owner_read |
                      std::filesystem::perms::owner_write)
                   : (std::filesystem::perms::owner_read |
                      std::filesystem::perms::owner_write |
                      std::filesystem::perms::group_read |
                      std::filesystem::perms::others_read),
      std::filesystem::perm_options::replace, ec);
  return true;
}

struct PasswordData {
  const std::string* password = nullptr;
};

int password_callback(char* buffer, int size, int, void* userdata) {
  auto* data = static_cast<PasswordData*>(userdata);
  if (data == nullptr || data->password == nullptr || size <= 0) return 0;
  const std::size_t len =
      std::min<std::size_t>(data->password->size(), static_cast<std::size_t>(size));
  std::memcpy(buffer, data->password->data(), len);
  return static_cast<int>(len);
}

std::string key_type_of(EVP_PKEY* key) {
  if (key == nullptr) return {};
  const int id = EVP_PKEY_base_id(key);
  if (id == EVP_PKEY_RSA) return "rsa";
  if (id == EVP_PKEY_EC) return "ec";
  if (id == EVP_PKEY_ED25519) return "ed25519";
  return "other";
}

nlohmann::json metadata(const std::string& id, const std::string& name,
                        const std::string& source, const nlohmann::json& info,
                        const std::string& key_type, int key_bits,
                        const std::filesystem::path& cert_path,
                        const std::filesystem::path& key_path) {
  return {{"id", id},
          {"name", name},
          {"source", source},
          {"createdAt", now_iso8601()},
          {"keyType", key_type},
          {"keyBits", key_bits},
          {"subject", info.value("subject", std::string())},
          {"issuer", info.value("issuer", std::string())},
          {"serial", info.value("serial", std::string())},
          {"notBefore", info.value("notBefore", std::string())},
          {"notAfter", info.value("notAfter", std::string())},
          {"expired", info.value("expired", false)},
          {"fingerprintSha256", info.value("fingerprintSha256", std::string())},
          {"certificateCount", info.value("certificateCount", 1)},
          {"certPath", cert_path.string()},
          {"keyPath", key_path.string()}};
}

}  // namespace

CertificateManager::CertificateManager(std::filesystem::path directory)
    : directory_(std::move(directory)) {}

nlohmann::json CertificateManager::list() const {
  nlohmann::json out = nlohmann::json::array();
  std::error_code ec;
  if (!std::filesystem::is_directory(directory_, ec)) {
    return {{"ok", true}, {"certificates", out}};
  }
  for (const auto& entry : std::filesystem::directory_iterator(directory_, ec)) {
    if (!entry.is_directory()) continue;
    const std::string id = entry.path().filename().string();
    if (!valid_id(id)) continue;
    const std::string text = read_file(entry.path() / "meta.json", 256 * 1024);
    if (text.empty()) continue;
    const auto meta = nlohmann::json::parse(text, nullptr, false);
    if (!meta.is_object()) continue;
    out.push_back(meta);
  }
  std::sort(out.begin(), out.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
    return a.value("createdAt", std::string()) > b.value("createdAt", std::string());
  });
  return {{"ok", true}, {"certificates", out}};
}

nlohmann::json CertificateManager::get(const std::string& id) const {
  if (!valid_id(id)) return {{"ok", false}, {"error", "certificate not found"}};
  const auto dir = directory_ / id;
  const std::string text = read_file(dir / "meta.json", 256 * 1024);
  const auto meta = nlohmann::json::parse(text, nullptr, false);
  if (!meta.is_object()) return {{"ok", false}, {"error", "certificate not found"}};
  const std::string pem = read_file(dir / "cert.pem", 4 * 1024 * 1024);
  nlohmann::json out = meta;
  if (!pem.empty()) {
    std::string error;
    auto info = inspect_certificate(pem, &error);
    if (info.is_object()) out["certificate"] = info;
  }
  return {{"ok", true}, {"certificate", out}};
}

nlohmann::json CertificateManager::generate(const GenerateCertificateInput& input,
                                            std::string* error) {
  const std::string name = trim(input.name).empty() ? "Generated certificate"
                                                    : trim(input.name);
  if (name.size() > 120) {
    if (error) *error = "name must be at most 120 characters";
    return nullptr;
  }
  const int days = std::max(1, std::min(input.days, 3650));
  if (input.key_type != "rsa" && input.key_type != "ec") {
    if (error) *error = "keyType must be rsa or ec";
    return nullptr;
  }
  std::string common_name = trim(input.common_name);
  std::vector<std::string> sans = input.sans;
  if (common_name.empty() && !sans.empty()) common_name = trim(sans.front());
  if (common_name.empty()) {
    if (error) *error = "commonName or at least one SAN is required";
    return nullptr;
  }
  if (sans.size() > 32) {
    if (error) *error = "at most 32 SAN entries are allowed";
    return nullptr;
  }
  bool cn_in_san = false;
  for (const auto& san : sans) {
    if (trim(san) == common_name) cn_in_san = true;
  }
  if (!cn_in_san) sans.insert(sans.begin(), common_name);
  const std::string san_text = build_san(sans);
  if (san_text.empty()) {
    if (error) *error = "no valid SAN entries";
    return nullptr;
  }

  PkeyPtr key = generate_key(input.key_type, input.key_bits);
  if (!key) {
    if (error) *error = "could not generate the private key";
    return nullptr;
  }
  X509Ptr cert(X509_new(), &X509_free);
  if (!cert) {
    if (error) *error = "could not allocate the certificate";
    return nullptr;
  }
  X509_set_version(cert.get(), 2);
  const std::string serial_hex = random_hex(8);
  BIGNUM* serial_bn = nullptr;
  BN_hex2bn(&serial_bn, serial_hex.c_str());
  ASN1_INTEGER* serial = X509_get_serialNumber(cert.get());
  if (serial_bn != nullptr) {
    BN_to_ASN1_INTEGER(serial_bn, serial);
    BN_free(serial_bn);
  }
  X509_gmtime_adj(X509_getm_notBefore(cert.get()), -300);
  X509_gmtime_adj(X509_getm_notAfter(cert.get()),
                  static_cast<long>(days) * 24 * 60 * 60);
  X509_set_pubkey(cert.get(), key.get());
  // `subject` is a macro in some Windows SDK header combinations; use a
  // distinct local name and inline the issuer update.
  X509_NAME* cert_name = X509_get_subject_name(cert.get());
  if (cert_name == nullptr ||
      X509_NAME_add_entry_by_txt(
          cert_name, "CN", MBSTRING_ASC,
          reinterpret_cast<const unsigned char*>(common_name.c_str()), -1, -1,
          0) != 1) {
    if (error) *error = "could not set the certificate subject";
    return nullptr;
  }
  X509_set_issuer_name(cert.get(), cert_name);

  X509V3_CTX ctx;
  X509V3_set_ctx_nodb(&ctx);
  X509V3_set_ctx(&ctx, cert.get(), cert.get(), nullptr, nullptr, 0);
  auto add_ext = [&](int nid, const std::string& value, bool critical) {
    X509_EXTENSION* ext =
        X509V3_EXT_conf_nid(nullptr, &ctx, nid, value.c_str());
    if (ext == nullptr) return false;
    if (critical) X509_EXTENSION_set_critical(ext, 1);
    const bool ok = X509_add_ext(cert.get(), ext, -1) == 1;
    X509_EXTENSION_free(ext);
    return ok;
  };
  if (!add_ext(NID_subject_alt_name, san_text, false) ||
      !add_ext(NID_basic_constraints, "critical,CA:FALSE", true) ||
      !add_ext(NID_key_usage,
               input.key_type == "ec" ? "critical,digitalSignature"
                                      : "critical,digitalSignature,keyEncipherment",
               true) ||
      !add_ext(NID_ext_key_usage, "serverAuth", false) ||
      !add_ext(NID_subject_key_identifier, "hash", false) ||
      !add_ext(NID_authority_key_identifier, "keyid:always", false)) {
    if (error) *error = "could not add certificate extensions";
    return nullptr;
  }
  if (X509_sign(cert.get(), key.get(), EVP_sha256()) <= 0) {
    if (error) *error = "could not sign the certificate";
    return nullptr;
  }

  BioPtr cert_bio(BIO_new(BIO_s_mem()), &BIO_free);
  BioPtr key_bio(BIO_new(BIO_s_mem()), &BIO_free);
  if (!cert_bio || !key_bio || PEM_write_bio_X509(cert_bio.get(), cert.get()) != 1 ||
      PEM_write_bio_PrivateKey(key_bio.get(), key.get(), nullptr, nullptr, 0,
                               nullptr, nullptr) != 1) {
    if (error) *error = "could not encode the certificate";
    return nullptr;
  }
  const std::string cert_pem = bio_string(cert_bio.get());
  const std::string key_pem = bio_string(key_bio.get());
  std::string parse_error;
  auto info = inspect_certificate(cert_pem, &parse_error);
  if (!info.is_object()) {
    if (error) *error = parse_error.empty() ? "generated certificate is invalid" : parse_error;
    return nullptr;
  }

  std::error_code ec;
  std::filesystem::create_directories(directory_, ec);
  std::filesystem::permissions(directory_, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace, ec);
  const std::string id = make_id();
  const auto dir = directory_ / id;
  std::filesystem::create_directories(dir, ec);
  std::filesystem::permissions(dir, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace, ec);
  const auto cert_path = dir / "cert.pem";
  const auto key_path = dir / "key.pem";
  if (!write_private_file(cert_path, cert_pem, false) ||
      !write_private_file(key_path, key_pem, true)) {
    if (error) *error = "could not write the certificate files";
    return nullptr;
  }
  const int bits = input.key_type == "ec" ? (input.key_bits == 384 ? 384 : 256)
                                          : EVP_PKEY_bits(key.get());
  auto meta = metadata(id, name, "generated", info, input.key_type, bits,
                       cert_path, key_path);
  write_private_file(dir / "meta.json", meta.dump(2), false);
  return {{"ok", true}, {"certificate", meta}};
}

nlohmann::json CertificateManager::import(const ImportCertificateInput& input,
                                          std::string* error) {
  const std::string cert_pem = trim(input.cert_pem);
  const std::string key_pem = trim(input.key_pem);
  if (cert_pem.find("BEGIN CERTIFICATE") == std::string::npos) {
    if (error) *error = "certPem does not contain a PEM certificate";
    return nullptr;
  }
  if (key_pem.find("PRIVATE KEY") == std::string::npos) {
    if (error) *error = "keyPem does not contain a PEM private key";
    return nullptr;
  }
  BioPtr cert_bio(BIO_new_mem_buf(cert_pem.data(), static_cast<int>(cert_pem.size())),
                  &BIO_free);
  X509Ptr leaf(cert_bio ? PEM_read_bio_X509(cert_bio.get(), nullptr, nullptr, nullptr)
                        : nullptr,
               &X509_free);
  if (!leaf) {
    if (error) *error = "could not parse the leaf certificate";
    return nullptr;
  }
  PasswordData password{&input.password};
  BioPtr key_bio(BIO_new_mem_buf(key_pem.data(), static_cast<int>(key_pem.size())),
                 &BIO_free);
  PkeyPtr key(key_bio ? PEM_read_bio_PrivateKey(
                            key_bio.get(), nullptr,
                            input.password.empty() ? nullptr : password_callback,
                            &password)
                      : nullptr,
              &EVP_PKEY_free);
  if (!key) {
    if (error) *error = "could not parse the private key (wrong password?)";
    return nullptr;
  }
  if (X509_check_private_key(leaf.get(), key.get()) != 1) {
    if (error) *error = "the private key does not match the certificate";
    return nullptr;
  }

  std::string parse_error;
  auto info = inspect_certificate(cert_pem, &parse_error);
  if (!info.is_object()) {
    if (error) *error = parse_error.empty() ? "invalid certificate" : parse_error;
    return nullptr;
  }
  BioPtr out_key_bio(BIO_new(BIO_s_mem()), &BIO_free);
  if (!out_key_bio ||
      PEM_write_bio_PrivateKey(out_key_bio.get(), key.get(), nullptr, nullptr, 0,
                               nullptr, nullptr) != 1) {
    if (error) *error = "could not encode the private key";
    return nullptr;
  }
  std::string normalized_cert = cert_pem;
  if (normalized_cert.empty() || normalized_cert.back() != '\n') normalized_cert.push_back('\n');

  std::error_code ec;
  std::filesystem::create_directories(directory_, ec);
  std::filesystem::permissions(directory_, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace, ec);
  const std::string id = make_id();
  const auto dir = directory_ / id;
  std::filesystem::create_directories(dir, ec);
  std::filesystem::permissions(dir, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace, ec);
  const auto cert_path = dir / "cert.pem";
  const auto key_path = dir / "key.pem";
  if (!write_private_file(cert_path, normalized_cert, false) ||
      !write_private_file(key_path, bio_string(out_key_bio.get()), true)) {
    if (error) *error = "could not write the certificate files";
    return nullptr;
  }
  std::string name = trim(input.name);
  if (name.empty()) name = info.value("subject", std::string("Imported certificate"));
  if (name.size() > 120) name = name.substr(0, 120);
  const std::string key_type = key_type_of(key.get());
  const int bits = EVP_PKEY_bits(key.get());
  auto meta = metadata(id, name, "imported", info, key_type, bits, cert_path, key_path);
  write_private_file(dir / "meta.json", meta.dump(2), false);
  return {{"ok", true}, {"certificate", meta}};
}

bool CertificateManager::remove(const std::string& id, std::string* error) {
  if (!valid_id(id) || !exists(id)) {
    if (error) *error = "certificate not found";
    return false;
  }
  std::error_code ec;
  std::filesystem::remove_all(directory_ / id, ec);
  if (ec) {
    if (error) *error = "could not remove the certificate: " + ec.message();
    return false;
  }
  return true;
}

std::optional<std::string> CertificateManager::cert_path(const std::string& id) const {
  if (!exists(id)) return std::nullopt;
  return (directory_ / id / "cert.pem").string();
}

std::optional<std::string> CertificateManager::key_path(const std::string& id) const {
  if (!exists(id)) return std::nullopt;
  return (directory_ / id / "key.pem").string();
}

bool CertificateManager::exists(const std::string& id) const {
  if (!valid_id(id)) return false;
  std::error_code ec;
  return std::filesystem::exists(directory_ / id / "cert.pem", ec) &&
         std::filesystem::exists(directory_ / id / "key.pem", ec);
}

}  // namespace uvscope
