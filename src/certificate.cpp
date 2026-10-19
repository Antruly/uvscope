#include "certificate.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <memory>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "crypto.h"
#include "util.h"

namespace uvscope {
namespace {

using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;
using X509Ptr = std::unique_ptr<X509, decltype(&X509_free)>;

std::string bio_to_string(BIO* bio) {
  if (bio == nullptr) return {};
  char* data = nullptr;
  const long len = BIO_get_mem_data(bio, &data);
  if (len <= 0 || data == nullptr) return {};
  return std::string(data, static_cast<std::size_t>(len));
}

std::string print_name(X509_NAME* name) {
  if (name == nullptr) return {};
  BioPtr bio(BIO_new(BIO_s_mem()), &BIO_free);
  if (!bio) return {};
  X509_NAME_print_ex(bio.get(), name, 0, XN_FLAG_RFC2253);
  return bio_to_string(bio.get());
}

std::string print_time(const ASN1_TIME* time) {
  if (time == nullptr) return {};
  BioPtr bio(BIO_new(BIO_s_mem()), &BIO_free);
  if (!bio) return {};
  ASN1_TIME_print(bio.get(), time);
  return bio_to_string(bio.get());
}

std::int64_t time_to_epoch(const ASN1_TIME* time) {
  if (time == nullptr) return 0;
  std::tm tm{};
  if (ASN1_TIME_to_tm(time, &tm) != 1) return 0;
#if defined(_WIN32)
  return static_cast<std::int64_t>(_mkgmtime(&tm));
#else
  return static_cast<std::int64_t>(timegm(&tm));
#endif
}

std::string serial_text(X509* cert) {
  if (cert == nullptr) return {};
  ASN1_INTEGER* serial = X509_get_serialNumber(cert);
  if (serial == nullptr) return {};
  BIGNUM* bn = ASN1_INTEGER_to_BN(serial, nullptr);
  if (bn == nullptr) return {};
  char* hex = BN_bn2hex(bn);
  std::string out = hex != nullptr ? std::string(hex) : std::string();
  OPENSSL_free(hex);
  BN_free(bn);
  return out;
}

std::string fingerprint(X509* cert) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  if (X509_digest(cert, EVP_sha256(), digest, &len) != 1) return {};
  std::string upper = hex_encode(std::string(reinterpret_cast<const char*>(digest), len));
  for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  std::string out;
  for (std::size_t i = 0; i < upper.size(); i += 2) {
    if (i) out.push_back(':');
    out.append(upper.substr(i, 2));
  }
  return out;
}

nlohmann::json san_list(X509* cert) {
  nlohmann::json out = nlohmann::json::array();
  GENERAL_NAMES* names = static_cast<GENERAL_NAMES*>(
      X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
  if (names == nullptr) return out;
  const int count = sk_GENERAL_NAME_num(names);
  for (int i = 0; i < count; ++i) {
    const GENERAL_NAME* name = sk_GENERAL_NAME_value(names, i);
    if (name == nullptr) continue;
    if (name->type == GEN_DNS) {
      const ASN1_STRING* dns = name->d.dNSName;
      const unsigned char* data = ASN1_STRING_get0_data(dns);
      const int len = ASN1_STRING_length(dns);
      if (data != nullptr && len > 0) {
        out.push_back({{"type", "DNS"}, {"value", std::string(reinterpret_cast<const char*>(data), len)}});
      }
    } else if (name->type == GEN_IPADD) {
      const ASN1_OCTET_STRING* ip = name->d.iPAddress;
      const unsigned char* data = ASN1_STRING_get0_data(ip);
      const int len = ASN1_STRING_length(ip);
      if (data != nullptr && (len == 4 || len == 16)) {
        char text[INET6_ADDRSTRLEN] = {};
#if defined(_WIN32)
        InetNtopA(len == 4 ? AF_INET : AF_INET6, data, text, sizeof(text));
#else
        inet_ntop(len == 4 ? AF_INET : AF_INET6, data, text, sizeof(text));
#endif
        out.push_back({{"type", "IP"}, {"value", text}});
      }
    } else if (name->type == GEN_EMAIL) {
      const ASN1_STRING* email = name->d.rfc822Name;
      const unsigned char* data = ASN1_STRING_get0_data(email);
      const int len = ASN1_STRING_length(email);
      if (data != nullptr && len > 0) {
        out.push_back({{"type", "email"}, {"value", std::string(reinterpret_cast<const char*>(data), len)}});
      }
    } else {
      BioPtr bio(BIO_new(BIO_s_mem()), &BIO_free);
      if (bio && GENERAL_NAME_print(bio.get(), const_cast<GENERAL_NAME*>(name)) == 1) {
        out.push_back({{"type", "other"}, {"value", bio_to_string(bio.get())}});
      }
    }
  }
  GENERAL_NAMES_free(names);
  return out;
}

nlohmann::json public_key(X509* cert) {
  nlohmann::json out = {{"algorithm", ""}, {"bits", 0}};
  EVP_PKEY* key = X509_get_pubkey(cert);
  if (key == nullptr) return out;
  const int id = EVP_PKEY_base_id(key);
  const char* name = OBJ_nid2ln(id);
  out["algorithm"] = name != nullptr ? name : "unknown";
  out["bits"] = EVP_PKEY_bits(key);
  EVP_PKEY_free(key);
  return out;
}

const char* key_usage_bit(int bit) {
  switch (bit) {
    case 0: return "digitalSignature";
    case 1: return "nonRepudiation";
    case 2: return "keyEncipherment";
    case 3: return "dataEncipherment";
    case 4: return "keyAgreement";
    case 5: return "keyCertSign";
    case 6: return "cRLSign";
    case 7: return "encipherOnly";
    case 8: return "decipherOnly";
    default: return nullptr;
  }
}

nlohmann::json key_usage(X509* cert) {
  nlohmann::json out = nlohmann::json::array();
  ASN1_BIT_STRING* usage = static_cast<ASN1_BIT_STRING*>(
      X509_get_ext_d2i(cert, NID_key_usage, nullptr, nullptr));
  if (usage == nullptr) return out;
  for (int i = 0; i < 9; ++i) {
    if (ASN1_BIT_STRING_get_bit(usage, i) != 0) {
      if (const char* name = key_usage_bit(i)) out.push_back(name);
    }
  }
  ASN1_BIT_STRING_free(usage);
  return out;
}

nlohmann::json ext_key_usage(X509* cert) {
  nlohmann::json out = nlohmann::json::array();
  EXTENDED_KEY_USAGE* usage = static_cast<EXTENDED_KEY_USAGE*>(
      X509_get_ext_d2i(cert, NID_ext_key_usage, nullptr, nullptr));
  if (usage == nullptr) return out;
  for (int i = 0; i < sk_ASN1_OBJECT_num(usage); ++i) {
    ASN1_OBJECT* obj = sk_ASN1_OBJECT_value(usage, i);
    char buffer[256] = {};
    OBJ_obj2txt(buffer, sizeof(buffer), obj, 0);
    out.push_back(buffer);
  }
  EXTENDED_KEY_USAGE_free(usage);
  return out;
}

}  // namespace

nlohmann::json inspect_certificate(const std::string& input, std::string* error) {
  if (trim(input).empty()) {
    if (error) *error = "certificate input is empty";
    return nullptr;
  }
  X509Ptr cert(nullptr, &X509_free);
  if (input.find("BEGIN CERTIFICATE") != std::string::npos) {
    BioPtr bio(BIO_new_mem_buf(input.data(), static_cast<int>(input.size())), &BIO_free);
    if (!bio) {
      if (error) *error = "failed to allocate certificate reader";
      return nullptr;
    }
    cert.reset(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
  } else {
    BioPtr bio(BIO_new_mem_buf(input.data(), static_cast<int>(input.size())), &BIO_free);
    if (!bio) {
      if (error) *error = "failed to allocate certificate reader";
      return nullptr;
    }
    cert.reset(d2i_X509_bio(bio.get(), nullptr));
  }
  if (!cert) {
    if (error) *error = "not a valid PEM or DER X.509 certificate";
    return nullptr;
  }

  const int version = static_cast<int>(X509_get_version(cert.get())) + 1;
  const ASN1_TIME* not_before = X509_get0_notBefore(cert.get());
  const ASN1_TIME* not_after = X509_get0_notAfter(cert.get());
  const std::int64_t not_before_epoch = time_to_epoch(not_before);
  const std::int64_t not_after_epoch = time_to_epoch(not_after);
  const std::int64_t now = now_unix();
  const bool self_signed = X509_NAME_cmp(X509_get_subject_name(cert.get()),
                                         X509_get_issuer_name(cert.get())) == 0;
  const int signature_nid = X509_get_signature_nid(cert.get());
  const char* signature_name = OBJ_nid2ln(signature_nid);

  nlohmann::json out;
  out["version"] = version;
  out["serial"] = serial_text(cert.get());
  out["subject"] = print_name(X509_get_subject_name(cert.get()));
  out["issuer"] = print_name(X509_get_issuer_name(cert.get()));
  out["notBefore"] = print_time(not_before);
  out["notAfter"] = print_time(not_after);
  out["notBeforeEpoch"] = not_before_epoch;
  out["notAfterEpoch"] = not_after_epoch;
  out["daysRemaining"] = not_after_epoch > 0 ? (not_after_epoch - now) / 86400 : 0;
  out["expired"] = not_after_epoch > 0 && now > not_after_epoch;
  out["notYetValid"] = not_before_epoch > 0 && now < not_before_epoch;
  out["selfSigned"] = self_signed;
  out["fingerprintSha256"] = fingerprint(cert.get());
  out["signatureAlgorithm"] = signature_name != nullptr ? signature_name : "unknown";
  out["publicKey"] = public_key(cert.get());
  out["san"] = san_list(cert.get());
  out["keyUsage"] = key_usage(cert.get());
  out["extendedKeyUsage"] = ext_key_usage(cert.get());
  out["sha256"] = sha256_hex(input);
  out["chainNotice"] = "v0.2.0 decodes the first certificate in the input. Paste the full chain to inspect the leaf first.";
  return out;
}

}  // namespace uvscope
