#include "routing.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include <uv.h>

#include "util.h"

namespace uvscope {
namespace {

constexpr std::size_t kMaxCustomCidrs = 100000;

std::size_t address_bytes(bool v6) { return v6 ? 16 : 4; }

int compare_addresses(const std::array<std::uint8_t, 16>& a,
                      const std::array<std::uint8_t, 16>& b, bool v6) {
  return std::memcmp(a.data(), b.data(), address_bytes(v6));
}

// Adds one to a big-endian address; returns false when it wraps to zero.
bool increment_address(std::array<std::uint8_t, 16>& value, bool v6) {
  const std::size_t n = address_bytes(v6);
  for (std::size_t i = n; i-- > 0;) {
    if (++value[i] != 0) return true;
  }
  return false;
}

bool range_before(const IpRange& a, const IpRange& b) {
  if (a.v6 != b.v6) return !a.v6;
  return compare_addresses(a.first, b.first, a.v6) < 0;
}

void normalize_ranges(std::vector<IpRange>* ranges) {
  if (ranges == nullptr || ranges->empty()) return;
  std::sort(ranges->begin(), ranges->end(), range_before);
  std::vector<IpRange> merged;
  merged.reserve(ranges->size());
  for (const auto& range : *ranges) {
    if (merged.empty() || merged.back().v6 != range.v6) {
      merged.push_back(range);
      continue;
    }
    IpRange& current = merged.back();
    bool joins = compare_addresses(range.first, current.last, current.v6) <= 0;
    if (!joins) {
      std::array<std::uint8_t, 16> after = current.last;
      if (increment_address(after, current.v6) &&
          compare_addresses(range.first, after, current.v6) <= 0) {
        joins = true;
      }
    }
    if (!joins) {
      merged.push_back(range);
      continue;
    }
    if (compare_addresses(range.last, current.last, current.v6) > 0) {
      current.last = range.last;
    }
  }
  ranges->swap(merged);
}

bool is_zero_prefix(const std::array<std::uint8_t, 16>& bytes, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    if (bytes[i] != 0) return false;
  }
  return true;
}

bool ranges_contain(const std::vector<IpRange>& ranges, const IpAddress& ip) {
  // Binary search for the last range whose first address is <= ip.
  std::size_t lo = 0;
  std::size_t hi = ranges.size();
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    const IpRange& range = ranges[mid];
    int cmp = 0;
    if (range.v6 != ip.v6) {
      cmp = range.v6 ? 1 : -1;
    } else {
      cmp = compare_addresses(range.first, ip.bytes, ip.v6);
    }
    if (cmp <= 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo == 0) return false;
  const IpRange& candidate = ranges[lo - 1];
  if (candidate.v6 != ip.v6) return false;
  return compare_addresses(candidate.last, ip.bytes, ip.v6) >= 0;
}

const AddressGroup* find_group(const std::vector<AddressGroup>& groups,
                               const std::string& id) {
  for (const auto& group : groups) {
    if (group.id == id) return &group;
  }
  return nullptr;
}

bool parse_proxy(const nlohmann::json& value, RoutingProxy* out,
                 std::string* error) {
  if (!value.is_object()) {
    if (error) *error = "proxy must be an object";
    return false;
  }
  if (value.contains("password")) {
    if (error) {
      *error =
          "upstream proxy credentials are not stored yet; "
          "leave the password out until the client release adds the secret store";
    }
    return false;
  }
  RoutingProxy proxy;
  proxy.protocol = value.value("protocol", std::string("socks5"));
  proxy.host = value.value("host", std::string());
  proxy.port = value.value("port", 0);
  proxy.username = value.value("username", std::string());
  if (proxy.protocol != "socks5" && proxy.protocol != "http") {
    if (error) *error = "proxy protocol must be socks5 or http";
    return false;
  }
  if (proxy.host.empty()) {
    if (error) *error = "proxy host is required";
    return false;
  }
  if (proxy.port <= 0 || proxy.port > 65535) {
    if (error) *error = "proxy port must be 1-65535";
    return false;
  }
  *out = std::move(proxy);
  return true;
}

std::string group_kind(const std::string& kind) {
  return kind == "complement" ? "complement" : "cidrs";
}

}  // namespace

bool parse_ip_address(const std::string& text, IpAddress* out,
                      std::string* error) {
  if (out == nullptr) return false;
  IpAddress parsed;
  struct sockaddr_in addr4;
  std::memset(&addr4, 0, sizeof(addr4));
  if (uv_ip4_addr(text.c_str(), 0, &addr4) == 0) {
    parsed.valid = true;
    std::memcpy(parsed.bytes.data(), &addr4.sin_addr, 4);
    *out = parsed;
    return true;
  }
  struct sockaddr_in6 addr6;
  std::memset(&addr6, 0, sizeof(addr6));
  if (uv_ip6_addr(text.c_str(), 0, &addr6) == 0) {
    unsigned char raw[16];
    std::memcpy(raw, &addr6.sin6_addr, 16);
    // ::ffff:a.b.c.d is the same host as a.b.c.d for routing purposes.
    static const unsigned char kV4Mapped[12] = {0, 0, 0, 0, 0, 0,
                                                0, 0, 0, 0, 0xff, 0xff};
    parsed.valid = true;
    if (std::memcmp(raw, kV4Mapped, sizeof(kV4Mapped)) == 0) {
      std::memcpy(parsed.bytes.data(), raw + 12, 4);
    } else {
      parsed.v6 = true;
      std::memcpy(parsed.bytes.data(), raw, 16);
    }
    *out = parsed;
    return true;
  }
  if (error) *error = "invalid IP address: " + text;
  return false;
}

bool parse_cidr_range(const std::string& text, IpRange* out,
                      std::string* error) {
  if (out == nullptr) return false;
  const std::size_t slash = text.find('/');
  if (slash == std::string::npos) {
    if (error) *error = "expected address/prefix: " + text;
    return false;
  }
  IpAddress address;
  if (!parse_ip_address(text.substr(0, slash), &address, error)) return false;
  const std::string prefix_text = text.substr(slash + 1);
  if (prefix_text.empty() ||
      prefix_text.size() > 3 ||
      !std::all_of(prefix_text.begin(), prefix_text.end(),
                   [](char c) { return c >= '0' && c <= '9'; })) {
    if (error) *error = "invalid prefix length: " + text;
    return false;
  }
  const int prefix = std::atoi(prefix_text.c_str());
  const int limit = address.v6 ? 128 : 32;
  if (prefix < 0 || prefix > limit) {
    if (error) *error = "prefix length out of range: " + text;
    return false;
  }

  IpRange range;
  range.v6 = address.v6;
  const std::size_t n = address_bytes(range.v6);
  for (std::size_t i = 0; i < n; ++i) {
    const int bits = prefix - static_cast<int>(i * 8);
    std::uint8_t first = address.bytes[i];
    std::uint8_t mask = 0;
    if (bits >= 8) {
      mask = 0xff;
    } else if (bits > 0) {
      mask = static_cast<std::uint8_t>(0xffu << (8 - bits));
    }
    range.first[i] = static_cast<std::uint8_t>(first & mask);
    range.last[i] = static_cast<std::uint8_t>(range.first[i] | ~mask);
  }
  *out = range;
  return true;
}

bool ip_in_range(const IpRange& range, const IpAddress& ip) {
  if (!ip.valid || range.v6 != ip.v6) return false;
  return compare_addresses(range.first, ip.bytes, ip.v6) <= 0 &&
         compare_addresses(range.last, ip.bytes, ip.v6) >= 0;
}

bool ip_is_public(const IpAddress& ip) {
  if (!ip.valid) return false;
  const auto& b = ip.bytes;
  if (!ip.v6) {
    const std::uint8_t a = b[0];
    const std::uint8_t c = b[1];
    if (a == 0 || a == 10 || a == 127) return false;
    if (a == 100 && (c & 0xc0) == 64) return false;   // 100.64/10 CGNAT
    if (a == 169 && c == 254) return false;           // link local
    if (a == 172 && (c & 0xf0) == 16) return false;   // 172.16/12
    if (a == 192 && c == 168) return false;           // 192.168/16
    if (a == 192 && c == 0 && b[2] == 0) return false;   // 192.0.0/24
    if (a == 192 && c == 0 && b[2] == 2) return false;   // TEST-NET-1
    if (a == 192 && c == 88 && b[2] == 99) return false; // 6to4 relay
    if (a == 198 && (c & 0xfe) == 18) return false;      // 198.18/15
    if (a == 198 && c == 51 && b[2] == 100) return false; // TEST-NET-2
    if (a == 203 && c == 0 && b[2] == 113) return false;  // TEST-NET-3
    if (a >= 224) return false;                            // multicast + reserved
    return true;
  }
  // IPv6
  if (is_zero_prefix(b, 12)) return false;  // ::/96 (IPv4-compatible, deprecated)
  static const std::uint8_t kLoopback[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                                             0, 0, 0, 0, 0, 0, 0, 1};
  if (std::memcmp(b.data(), kLoopback, 16) == 0) return false;
  if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b) return false;  // NAT64
  // 100::/64 (discard prefix): 0x01 0x00 then six zero bytes.
  if (b[0] == 0x01 && b[1] == 0x00 && is_zero_prefix(b, 8)) return false;
  if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8) return false;  // 2001:db8::/32
  if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x00 && (b[3] & 0xf0) == 0x10) return false;  // ORCHID
  if ((b[0] & 0xfe) == 0xfc) return false;                  // fc00::/7 ULA
  if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return false;  // fe80::/10
  if (b[0] == 0xff) return false;                           // multicast
  return true;
}

AddressGroup make_loopback_group() {
  AddressGroup group;
  group.id = "loopback";
  group.name = "Loopback";
  group.builtin = true;
  group.kind = "cidrs";
  group.description = "127.0.0.0/8 and ::1";
  group.source = "built-in";
  for (const char* cidr : {"127.0.0.0/8", "::1/128"}) {
    IpRange range;
    if (parse_cidr_range(cidr, &range, nullptr)) group.ranges.push_back(range);
  }
  normalize_ranges(&group.ranges);
  return group;
}

namespace {

AddressGroup make_mainland_group() {
  AddressGroup group;
  group.id = "mainland";
  group.name = "Mainland China";
  group.builtin = true;
  group.kind = "cidrs";
  group.description = "APNIC allocations registered to CN";
  group.source = kGeoMainlandSource;
  std::istringstream input(kGeoMainlandText);
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#') continue;
    const std::size_t dash = line.find('-');
    if (dash == std::string::npos) continue;
    IpAddress first;
    IpAddress last;
    if (!parse_ip_address(line.substr(0, dash), &first, nullptr)) continue;
    if (!parse_ip_address(line.substr(dash + 1), &last, nullptr)) continue;
    if (first.v6 != last.v6) continue;
    IpRange range;
    range.v6 = first.v6;
    range.first = first.bytes;
    range.last = last.bytes;
    group.ranges.push_back(range);
  }
  normalize_ranges(&group.ranges);
  return group;
}

}  // namespace

const std::vector<AddressGroup>& builtin_address_groups() {
  static const std::vector<AddressGroup> builtins = [] {
    std::vector<AddressGroup> out;
    out.push_back(make_mainland_group());
    AddressGroup overseas;
    overseas.id = "overseas";
    overseas.name = "Overseas";
    overseas.builtin = true;
    overseas.kind = "complement";
    overseas.complementOf = "mainland";
    overseas.description =
        "Public addresses outside the mainland group (private ranges do not match)";
    overseas.source = "built-in (complement of mainland)";
    out.push_back(std::move(overseas));
    out.push_back(make_loopback_group());
    return out;
  }();
  return builtins;
}

bool group_contains(const std::vector<AddressGroup>& groups,
                    const AddressGroup& group, const IpAddress& ip) {
  if (!ip.valid) return false;
  if (group.kind == "complement") {
    if (!ip_is_public(ip)) return false;
    const AddressGroup* base = find_group(groups, group.complementOf);
    if (base == nullptr) return false;
    return !ranges_contain(base->ranges, ip);
  }
  return ranges_contain(group.ranges, ip);
}

std::vector<std::string> matching_group_ids(
    const std::vector<AddressGroup>& groups, const IpAddress& ip) {
  std::vector<std::string> out;
  for (const auto& group : groups) {
    if (group_contains(groups, group, ip)) out.push_back(group.id);
  }
  return out;
}

RouteDecision route_decide(const RoutingConfig& config,
                           const std::vector<AddressGroup>& groups,
                           const IpAddress& ip) {
  RouteDecision decision;
  for (const auto& rule : config.rules) {
    const AddressGroup* group = find_group(groups, rule.groupId);
    if (group == nullptr) continue;
    if (!group_contains(groups, *group, ip)) continue;
    decision.matched = true;
    decision.groupId = rule.groupId;
    decision.ruleId = rule.id;
    decision.action = rule.action;
    decision.proxy = rule.proxy;
    decision.reason = "rule";
    return decision;
  }
  decision.action = config.defaultAction;
  decision.proxy = config.defaultProxy;
  decision.reason = "default";
  return decision;
}

nlohmann::json routing_proxy_json(const RoutingProxy& proxy) {
  return {{"protocol", proxy.protocol},
          {"host", proxy.host},
          {"port", proxy.port},
          {"username", proxy.username}};
}

nlohmann::json address_group_json(const AddressGroup& group) {
  nlohmann::json out = {{"id", group.id},
                        {"name", group.name},
                        {"builtin", group.builtin},
                        {"kind", group_kind(group.kind)},
                        {"description", group.description},
                        {"source", group.source},
                        {"rangeCount", group.ranges.size()},
                        {"available", true}};
  if (group.builtin) {
    out["available"] = !group.ranges.empty() || group.kind == "complement";
    out["complementOf"] = group.complementOf;
  } else {
    out["cidrs"] = group.cidrs;
  }
  return out;
}

nlohmann::json routing_config_json(const RoutingConfig& config) {
  nlohmann::json rules = nlohmann::json::array();
  for (const auto& rule : config.rules) {
    nlohmann::json entry = {{"id", rule.id},
                            {"groupId", rule.groupId},
                            {"action", rule.action}};
    if (rule.action == "proxy") entry["proxy"] = routing_proxy_json(rule.proxy);
    rules.push_back(std::move(entry));
  }
  nlohmann::json out = {{"rules", rules},
                        {"default",
                         {{"action", config.defaultAction},
                          {"proxy", routing_proxy_json(config.defaultProxy)}}}};
  return out;
}

bool custom_group_from_json(const nlohmann::json& value, AddressGroup* out,
                            std::string* error) {
  if (out == nullptr) return false;
  if (!value.is_object()) {
    if (error) *error = "address group must be a JSON object";
    return false;
  }
  AddressGroup group;
  group.id = value.value("id", std::string());
  group.name = value.value("name", std::string());
  group.builtin = false;
  group.kind = "cidrs";
  group.description = value.value("description", std::string());
  group.source = "custom";
  const auto cidrs = value.value("cidrs", nlohmann::json::array());
  if (group.name.empty()) {
    if (error) *error = "group name is required";
    return false;
  }
  if (group.name.size() > 64) {
    if (error) *error = "group name is too long";
    return false;
  }
  if (!cidrs.is_array() || cidrs.empty()) {
    if (error) *error = "at least one CIDR is required";
    return false;
  }
  if (cidrs.size() > kMaxCustomCidrs) {
    if (error) *error = "too many CIDRs";
    return false;
  }
  for (const auto& item : cidrs) {
    if (!item.is_string()) {
      if (error) *error = "CIDRs must be strings";
      return false;
    }
    const std::string text = item.get<std::string>();
    IpRange range;
    std::string cidr_error;
    if (!parse_cidr_range(text, &range, &cidr_error)) {
      if (error) *error = cidr_error;
      return false;
    }
    group.cidrs.push_back(text);
    group.ranges.push_back(range);
  }
  normalize_ranges(&group.ranges);
  *out = std::move(group);
  return true;
}

bool custom_groups_from_json(const nlohmann::json& value,
                             std::vector<AddressGroup>* out,
                             std::string* error) {
  if (out == nullptr) return false;
  if (!value.is_array()) {
    if (error) *error = "address groups must be an array";
    return false;
  }
  std::vector<AddressGroup> groups;
  for (const auto& item : value) {
    AddressGroup group;
    if (!custom_group_from_json(item, &group, error)) return false;
    if (group.id.empty()) {
      if (error) *error = "address group id is missing";
      return false;
    }
    groups.push_back(std::move(group));
  }
  *out = std::move(groups);
  return true;
}

nlohmann::json custom_groups_to_json(const std::vector<AddressGroup>& groups) {
  nlohmann::json out = nlohmann::json::array();
  for (const auto& group : groups) {
    out.push_back({{"id", group.id},
                   {"name", group.name},
                   {"description", group.description},
                   {"cidrs", group.cidrs}});
  }
  return out;
}

bool routing_config_from_json(const nlohmann::json& value,
                              const std::vector<AddressGroup>& groups,
                              RoutingConfig* out, std::string* error) {
  if (out == nullptr) return false;
  if (!value.is_object()) {
    if (error) *error = "routing config must be a JSON object";
    return false;
  }
  RoutingConfig config;
  const auto rules = value.value("rules", nlohmann::json::array());
  if (!rules.is_array()) {
    if (error) *error = "rules must be an array";
    return false;
  }
  for (const auto& item : rules) {
    if (!item.is_object()) {
      if (error) *error = "each rule must be an object";
      return false;
    }
    RouteRule rule;
    rule.id = item.value("id", std::string());
    rule.groupId = item.value("groupId", std::string());
    rule.action = item.value("action", std::string("direct"));
    if (rule.groupId.empty() || find_group(groups, rule.groupId) == nullptr) {
      if (error) *error = "rule references an unknown address group: " + rule.groupId;
      return false;
    }
    if (rule.action != "direct" && rule.action != "proxy") {
      if (error) *error = "rule action must be direct or proxy";
      return false;
    }
    if (rule.action == "proxy") {
      if (!parse_proxy(item.value("proxy", nlohmann::json::object()),
                       &rule.proxy, error)) {
        return false;
      }
    }
    if (rule.id.empty()) rule.id = make_id();
    config.rules.push_back(std::move(rule));
  }

  const auto fallback = value.value("default", nlohmann::json::object());
  if (!fallback.is_object()) {
    if (error) *error = "default must be an object";
    return false;
  }
  config.defaultAction = fallback.value("action", std::string("direct"));
  if (config.defaultAction != "direct" && config.defaultAction != "proxy") {
    if (error) *error = "default action must be direct or proxy";
    return false;
  }
  if (config.defaultAction == "proxy") {
    if (!parse_proxy(fallback.value("proxy", nlohmann::json::object()),
                     &config.defaultProxy, error)) {
      return false;
    }
  }
  *out = std::move(config);
  return true;
}

}  // namespace uvscope
