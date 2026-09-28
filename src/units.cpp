#include "cooling_failover/units.hpp"

#include <limits>

namespace cooling_failover {
namespace {

constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kInt64Min = std::numeric_limits<std::int64_t>::min();

[[nodiscard]] std::string decimal(std::int64_t value) {
  if (value == 0) {
    return "0";
  }
  const bool negative = value < 0;
  std::uint64_t magnitude = negative
                                ? static_cast<std::uint64_t>(0) - static_cast<std::uint64_t>(value)
                                : static_cast<std::uint64_t>(value);
  char digits[24] = {};
  std::size_t used = 0;
  while (magnitude != 0 && used < sizeof(digits)) {
    digits[used++] = static_cast<char>('0' + static_cast<int>(magnitude % 10U));
    magnitude /= 10U;
  }
  std::string out;
  out.reserve(used + 1);
  if (negative) {
    out.push_back('-');
  }
  for (std::size_t i = used; i > 0; --i) {
    out.push_back(digits[i - 1]);
  }
  return out;
}

}  // namespace

Result<BasisPoints> BasisPoints::from_value(std::int32_t value) {
  if (value < 0 || value > scale()) {
    return Status::error(ErrorCode::OutOfRange, "basis points outside 0..10000", "value",
                         static_cast<std::int64_t>(value));
  }
  return BasisPoints(value);
}

Result<Watts> Watts::add(Watts a, Watts b) {
  if (b.value_ > 0 && a.value_ > kInt64Max - b.value_) {
    return Status::error(ErrorCode::Overflow, "cooling capacity addition overflowed", "lhs", a.value_,
                         "rhs", b.value_);
  }
  if (b.value_ < 0 && a.value_ < kInt64Min - b.value_) {
    return Status::error(ErrorCode::Overflow, "cooling capacity addition underflowed", "lhs", a.value_,
                         "rhs", b.value_);
  }
  return Watts(a.value_ + b.value_);
}

Result<Watts> Watts::subtract(Watts a, Watts b) {
  if (b.value_ == kInt64Min) {
    if (a.value_ >= 0) {
      return Status::error(ErrorCode::Overflow, "cooling capacity subtraction overflowed");
    }
    return Watts(a.value_ - b.value_);
  }
  return add(a, Watts(-b.value_));
}

Result<Watts> Watts::sum(const std::int64_t* values, std::size_t count) {
  if (values == nullptr && count != 0) {
    return Status::error(ErrorCode::InvalidArgument, "capacity sum source is null");
  }
  Watts accumulator = Watts::zero();
  for (std::size_t i = 0; i < count; ++i) {
    Result<Watts> next = add(accumulator, Watts(values[i]));
    if (!next.has_value()) {
      Status status = next.status();
      status.with("index", static_cast<std::uint64_t>(i));
      return status;
    }
    accumulator = *next;
  }
  return accumulator;
}

Result<Watts> Watts::scale_up(Watts a, BasisPoints basis_points) {
  const std::int32_t bp = basis_points.value();
  if (bp < 0 || bp > BasisPoints::scale()) {
    return Status::error(ErrorCode::OutOfRange, "basis points outside 0..10000");
  }
  if (bp == 0 || a.value_ == 0) {
    return Watts::zero();
  }
  const bool negative = a.value_ < 0;
  const std::uint64_t magnitude =
      negative ? static_cast<std::uint64_t>(0) - static_cast<std::uint64_t>(a.value_)
               : static_cast<std::uint64_t>(a.value_);

  // 128-bit magnitude * bp, computed with 32-bit limbs so no step overflows.
  const std::uint64_t low_limb = magnitude & 0xFFFFFFFFULL;
  const std::uint64_t high_limb = magnitude >> 32;
  const std::uint64_t product_low = low_limb * static_cast<std::uint64_t>(bp);
  const std::uint64_t product_high = high_limb * static_cast<std::uint64_t>(bp);
  const std::uint64_t low = product_low + (product_high << 32);
  const std::uint64_t carry = (low < product_low) ? 1ULL : 0ULL;
  const std::uint64_t high = (product_high >> 32) + carry;

  constexpr std::uint64_t kDivisor = 10000;
  if (high >= kDivisor) {
    // Cannot occur for bp <= 10000 and any magnitude; guarded anyway so that the
    // long division below is always valid.
    return Status::error(ErrorCode::Overflow, "capacity scaling overflowed", "watts", a.value_);
  }

  std::uint64_t quotient = 0;
  std::uint64_t remainder = high;
  for (int bit = 63; bit >= 0; --bit) {
    remainder = (remainder << 1) | ((low >> bit) & 1ULL);
    if (remainder >= kDivisor) {
      remainder -= kDivisor;
      quotient |= (1ULL << bit);
    }
  }
  if (remainder != 0) {
    if (quotient == std::numeric_limits<std::uint64_t>::max()) {
      return Status::error(ErrorCode::Overflow, "capacity scaling overflowed", "watts", a.value_);
    }
    ++quotient;  // round the magnitude away from zero
  }

  if (negative) {
    constexpr std::uint64_t kNegativeLimit = static_cast<std::uint64_t>(kInt64Max) + 1ULL;
    if (quotient > kNegativeLimit) {
      return Status::error(ErrorCode::Overflow, "scaled capacity underflowed", "watts", a.value_);
    }
    if (quotient == kNegativeLimit) {
      return Watts(kInt64Min);
    }
    return Watts(-static_cast<std::int64_t>(quotient));
  }
  if (quotient > static_cast<std::uint64_t>(kInt64Max)) {
    return Status::error(ErrorCode::Overflow, "scaled capacity overflowed", "watts", a.value_);
  }
  return Watts(static_cast<std::int64_t>(quotient));
}

std::string Watts::to_string() const { return decimal(value_) + "W"; }

}  // namespace cooling_failover
