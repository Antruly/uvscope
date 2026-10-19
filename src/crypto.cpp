#include "crypto.h"

#include <climits>
#include <cstdlib>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include "util.h"

namespace uvscope {
namespace {

constexpr int kIterations = 210000;
constexpr std::size_t kSaltBytes = 16;
constexpr std::size_t kHashBytes = 32;

std::vector<unsigned char> derive(const std::string& password, const std::vector<unsigned char>& salt, int iterations) {
  std::vector<unsigned char> out(kHashBytes);
  if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), salt.data(),
                        static_cast<int>(salt.size()), iterations, EVP_sha256(),
                        static_cast<int>(out.size()), out.data()) != 1) {
    out.assign(kHashBytes, 0);
  }
  return out;
}

}  // namespace

std::string password_hash(const std::string& password) {
  std::vector<unsigned char> salt(kSaltBytes);
  if (!secure_random_bytes(salt.data(), salt.size())) return {};
  const auto hash = derive(password, salt, kIterations);
  const std::string salt_b64 = base64_encode(std::string(reinterpret_cast<const char*>(salt.data()), salt.size()));
  const std::string hash_b64 = base64_encode(std::string(reinterpret_cast<const char*>(hash.data()), hash.size()));
  return "pbkdf2-sha256$" + std::to_string(kIterations) + "$" + salt_b64 + "$" + hash_b64;
}

bool password_verify(const std::string& password, const std::string& encoded) {
  if (encoded.rfind("pbkdf2-sha256$", 0) != 0) return false;
  const std::size_t first = encoded.find('$');
  const std::size_t second = encoded.find('$', first + 1);
  const std::size_t third = encoded.find('$', second + 1);
  if (first == std::string::npos || second == std::string::npos || third == std::string::npos) return false;
  const int iterations = std::atoi(encoded.substr(first + 1, second - first - 1).c_str());
  if (iterations < 10000 || iterations > 2000000) return false;
  const auto salt = base64_decode(encoded.substr(second + 1, third - second - 1));
  const auto expected = base64_decode(encoded.substr(third + 1));
  if (!salt || !expected || expected->size() != kHashBytes) return false;
  const auto actual = derive(password, std::vector<unsigned char>(salt->begin(), salt->end()), iterations);
  return CRYPTO_memcmp(actual.data(), expected->data(), actual.size()) == 0;
}

bool password_needs_rehash(const std::string& encoded) {
  return encoded.rfind("pbkdf2-sha256$" + std::to_string(kIterations) + "$", 0) != 0;
}

std::string sha256_hex(const std::string& bytes) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), digest);
  return hex_encode(std::string(reinterpret_cast<const char*>(digest), sizeof(digest)));
}

bool secure_random_bytes(unsigned char* out, std::size_t len) {
  return out != nullptr && len <= static_cast<std::size_t>(INT_MAX) &&
         RAND_bytes(out, static_cast<int>(len)) == 1;
}

}  // namespace uvscope
