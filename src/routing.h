#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace uvscope {

// One parsed IP address. IPv4-mapped IPv6 literals are normalised to IPv4 so a
// rule written for 1.2.3.4 also matches ::ffff:1.2.3.4.
struct IpAddress {
  bool v6 = false;
  bool valid = false;
  std::array<std::uint8_t, 16> bytes{};
};

// Inclusive address range in the same normalised form as IpAddress.
struct IpRange {
  bool v6 = false;
  std::array<std::uint8_t, 16> first{};
  std::array<std::uint8_t, 16> last{};
};

// A named set of addresses. Custom groups are CIDR lists; the built-in
// `overseas` group is a complement predicate (`complement_of` names the base
// group) so it never has to materialise "everything else".
struct AddressGroup {
  std::string id;
  std::string name;
  bool builtin = false;
  std::string kind = "cidrs";  // "cidrs" | "complement"
  std::string complementOf;
  std::string description;
  std::string source;
  std::vector<IpRange> ranges;
  // Original CIDR text for custom groups (the built-ins keep an empty list).
  std::vector<std::string> cidrs;
};

// Upstream proxy referenced by a rule. Phase 1 stores no credentials; the
// client work adds the secret store before these are dialled.
struct RoutingProxy {
  std::string protocol = "socks5";  // "socks5" | "http"
  std::string host;
  int port = 0;
  std::string username;
};

struct RouteRule {
  std::string id;
  std::string groupId;
  std::string action = "direct";  // "direct" | "proxy"
  RoutingProxy proxy;
};

struct RoutingConfig {
  std::vector<RouteRule> rules;
  std::string defaultAction = "direct";  // "direct" | "proxy"
  RoutingProxy defaultProxy;
};

struct RouteDecision {
  bool matched = false;
  std::string groupId;
  std::string ruleId;
  std::string action = "direct";
  RoutingProxy proxy;
  std::string reason = "default";  // "rule" | "default"
};

// Embedded APNIC-derived mainland ranges (generated at build time from
// data/geo/cn-cidrs.txt; see scripts/gen-geo-cidrs.py).
extern const char* const kGeoMainlandText;
extern const char* const kGeoMainlandSource;

bool parse_ip_address(const std::string& text, IpAddress* out, std::string* error);
bool parse_cidr_range(const std::string& text, IpRange* out, std::string* error);
bool ip_in_range(const IpRange& range, const IpAddress& ip);
bool ip_is_public(const IpAddress& ip);

// Built-in groups, in rule-independent order: mainland, overseas, loopback.
const std::vector<AddressGroup>& builtin_address_groups();
AddressGroup make_loopback_group();

// Whether `ip` belongs to `group`. Complement groups consult the group named
// by `complementOf` inside `groups`.
bool group_contains(const std::vector<AddressGroup>& groups,
                    const AddressGroup& group, const IpAddress& ip);

// First matching rule wins; otherwise the configured default action applies.
RouteDecision route_decide(const RoutingConfig& config,
                           const std::vector<AddressGroup>& groups,
                           const IpAddress& ip);

// All groups that contain `ip`, in the order they appear in `groups`.
std::vector<std::string> matching_group_ids(
    const std::vector<AddressGroup>& groups, const IpAddress& ip);

nlohmann::json address_group_json(const AddressGroup& group);
nlohmann::json routing_proxy_json(const RoutingProxy& proxy);
nlohmann::json routing_config_json(const RoutingConfig& config);

// Validates one custom group (`{name, cidrs[]}`); an id is required.
bool custom_group_from_json(const nlohmann::json& value, AddressGroup* out,
                            std::string* error);
// Validates the persisted custom-group list.
bool custom_groups_from_json(const nlohmann::json& value,
                             std::vector<AddressGroup>* out, std::string* error);
nlohmann::json custom_groups_to_json(const std::vector<AddressGroup>& groups);

// Validates rules against the available groups.
bool routing_config_from_json(const nlohmann::json& value,
                              const std::vector<AddressGroup>& groups,
                              RoutingConfig* out, std::string* error);

}  // namespace uvscope
