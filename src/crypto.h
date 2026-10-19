#pragma once

#include <string>

namespace uvscope {

std::string password_hash(const std::string& password);
bool password_verify(const std::string& password, const std::string& encoded);
bool password_needs_rehash(const std::string& encoded);
std::string sha256_hex(const std::string& bytes);
bool secure_random_bytes(unsigned char* out, std::size_t len);

}  // namespace uvscope
