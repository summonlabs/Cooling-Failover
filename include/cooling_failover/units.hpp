// Cooling Failover - exact integer physical quantities.
//
// No floating point is used for any authority or accounting boundary.
#pragma once

#include "cooling_failover/export.hpp"
#include "cooling_failover/status.hpp"

#include <compare>
#include <cstdint>
#include <string>

namespace cooling_failover {

/// Basis points: hundredths of a percent. Valid range 0..10000 inclusive.
class [[nodiscard]] BasisPoints {
 public:
  constexpr BasisPoints() noexcept = default;
  explicit constexpr BasisPoints(std::int32_t value) noexcept : value_(value) {}

  [[nodiscard]] static Result<BasisPoints> from_value(std::int32_t value);
  [[nodiscard]] static constexpr BasisPoints zero() noexcept { return BasisPoints(0); }
  [[nodiscard]] static constexpr std::int32_t scale() noexcept { return 10000; }

  [[nodiscard]] constexpr std::int32_t value() const noexcept { return value_; }

  friend constexpr auto operator<=>(const BasisPoints&, const BasisPoints&) noexcept = default;

 private:
  std::int32_t value_{0};
};

/// Active cooling capacity in whole watts. Signed so that deficits can be
/// represented explicitly rather than underflowing.
class [[nodiscard]] Watts {
 public:
  constexpr Watts() noexcept = default;
  explicit constexpr Watts(std::int64_t value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr Watts from_watts(std::int64_t value) noexcept { return Watts(value); }
  [[nodiscard]] static constexpr Watts zero() noexcept { return Watts(0); }
  [[nodiscard]] static constexpr std::int64_t max_watts() noexcept { return kMax; }
  [[nodiscard]] static constexpr std::int64_t min_watts() noexcept { return kMin; }

  [[nodiscard]] constexpr std::int64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }
  [[nodiscard]] constexpr bool is_negative() const noexcept { return value_ < 0; }

  [[nodiscard]] static Result<Watts> add(Watts a, Watts b);
  [[nodiscard]] static Result<Watts> subtract(Watts a, Watts b);
  [[nodiscard]] static Result<Watts> sum(const std::int64_t* values, std::size_t count);

  /// \p a scaled by \p basis_points / 10000, rounded up, so that a required
  /// margin is never silently rounded down to zero.
  [[nodiscard]] static Result<Watts> scale_up(Watts a, BasisPoints basis_points);

  [[nodiscard]] std::string to_string() const;

  friend constexpr auto operator<=>(const Watts&, const Watts&) noexcept = default;

 private:
  static constexpr std::int64_t kMax = 9223372036854775807LL;
  static constexpr std::int64_t kMin = -9223372036854775807LL - 1;

  std::int64_t value_{0};
};

/// Count of independent source groups. Distinct from a plain integer so that a
/// group count cannot be confused with a capacity or an identity.
class [[nodiscard]] GroupCount {
 public:
  constexpr GroupCount() noexcept = default;
  explicit constexpr GroupCount(std::uint32_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_; }

  friend constexpr auto operator<=>(const GroupCount&, const GroupCount&) noexcept = default;

 private:
  std::uint32_t value_{0};
};

}  // namespace cooling_failover
