#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace uvscope {

// Parses the first certificate in a PEM bundle, or a DER certificate.
// The returned object is ready to be embedded in an API response.
nlohmann::json inspect_certificate(const std::string& input, std::string* error);

}  // namespace uvscope
