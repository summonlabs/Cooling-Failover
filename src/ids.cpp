#include "cooling_failover/ids.hpp"

namespace cooling_failover {
namespace detail {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

[[nodiscard]] bool is_hex_digit(char c) noexcept {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

[[nodiscard]] std::uint32_t hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return static_cast<std::uint32_t>(c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return static_cast<std::uint32_t>(c - 'a') + 10U;
  }
  return static_cast<std::uint32_t>(c - 'A') + 10U;
}

[[nodiscard]] bool is_decimal_digit(char c) noexcept { return c >= '0' && c <= '9'; }

}  // namespace

std::string hex64(std::uint64_t value) {
  std::string out;
  out.reserve(18);
  out.push_back('0');
  out.push_back('x');
  for (int shift = 60; shift >= 0; shift -= 4) {
    out.push_back(kHexDigits[(value >> shift) & 0xFU]);
  }
  return out;
}

Result<std::uint64_t> parse_u64(std::string_view text, std::string_view field) {
  const std::string field_name(field);
  if (text.empty()) {
    return Status::error(ErrorCode::EmptyRequiredField, "identity text is empty", "field", field_name);
  }
  if (field.empty()) {
    return Status::error(ErrorCode::InvalidArgument, "identity field name is empty");
  }
  constexpr std::size_t kMaxTextLength = 20;  // 20 decimal digits, or "0x" + 16 hex digits
  if (text.size() > kMaxTextLength) {
    return Status::error(ErrorCode::MalformedIdentity, "identity text is too long", "field", field_name);
  }
  for (char c : text) {
    if (static_cast<unsigned char>(c) < 0x21U || static_cast<unsigned char>(c) > 0x7EU) {
      return Status::error(ErrorCode::MalformedIdentity,
                           "identity text contains non-printable or non-ASCII bytes", "field",
                           field_name);
    }
  }

  std::size_t index = 0;
  bool hexadecimal = false;
  if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    hexadecimal = true;
    index = 2;
    if (text.size() == 2) {
      return Status::error(ErrorCode::MalformedIdentity, "identity text has no digits", "field",
                           field_name);
    }
  } else if (text.size() > 1 && text[0] == '0') {
    return Status::error(ErrorCode::MalformedIdentity,
                         "identity text has a redundant leading zero", "field", field_name);
  }

  std::uint64_t value = 0;
  for (; index < text.size(); ++index) {
    const char c = text[index];
    std::uint64_t digit = 0;
    if (hexadecimal) {
      if (!is_hex_digit(c)) {
        return Status::error(ErrorCode::MalformedIdentity,
                             "identity text is not valid hexadecimal", "field", field_name);
      }
      digit = hex_value(c);
    } else {
      if (!is_decimal_digit(c)) {
        return Status::error(ErrorCode::MalformedIdentity,
                             "identity text is not a valid decimal literal", "field", field_name);
      }
      digit = static_cast<std::uint64_t>(c - '0');
    }
    const std::uint64_t base = hexadecimal ? 16U : 10U;
    // Checked accumulation: reject anything that would not fit in 64 bits.
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / base) {
      return Status::error(ErrorCode::OutOfRange, "identity text does not fit in 64 bits", "field",
                           field_name);
    }
    value = value * base + digit;
  }
  return value;
}

}  // namespace detail
}  // namespace cooling_failover
