// Cooling Failover - machine-readable status and error contract.
//
// Every public operation returns an explicit Status/ErrorCode. Exception text is
// never the machine contract.
#pragma once

#include "cooling_failover/export.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace cooling_failover {

/// Stable machine-readable error codes.
///
/// The numeric value is the contract; the symbolic name may be extended but the
/// meaning of an existing value never changes.
enum class ErrorCode : std::uint16_t {
  Ok = 0,

  // 1xx - input shape and bounds.
  InvalidArgument = 100,
  MalformedIdentity = 101,
  OutOfRange = 102,
  BoundsExceeded = 103,
  DuplicateIdentity = 104,
  EmptyRequiredField = 105,
  InvalidEnumValue = 106,
  InvalidText = 107,
  InvalidPath = 110,
  PathTooLong = 111,
  PathAmbiguous = 112,
  PathReparsePoint = 113,

  // 2xx - generation, epoch and authority binding.
  StaleGeneration = 200,
  FutureGeneration = 201,
  SupersededEpoch = 202,
  EpochMismatch = 203,
  EpochNotEstablished = 204,
  StalePlan = 210,
  PlanFenced = 211,
  PlanNotFound = 212,
  PlanAlreadyActive = 213,
  PlanNotActive = 214,
  AuthorityDenied = 220,
  AuthorityUnknown = 221,
  AuthorityMissing = 222,
  AuthoritySuperseded = 223,

  // 3xx - evidence currency.
  EvidenceMissing = 300,
  EvidenceStale = 301,
  EvidenceFuture = 302,
  EvidenceGenerationMismatch = 303,
  EvidenceIndeterminate = 304,
  EvidenceUnsupported = 305,
  EvidenceKindMismatch = 306,
  EvidenceOwnerMismatch = 307,
  EvidenceRecoveredStale = 308,
  EvidenceReordered = 309,

  // 4xx - eligibility, redundancy and selection.
  NoEligibleCandidate = 400,
  SelectionNotDefined = 401,
  PolicyNotDefined = 402,
  PolicyMismatch = 403,
  DuplicateReserveUse = 410,
  InsufficientHeadroom = 411,
  InsufficientIndependence = 412,
  ObligationUnprotected = 413,
  CandidateStale = 414,
  TopologyNotImported = 415,

  // 5xx - transition.
  AttemptNotFound = 500,
  AttemptAlreadyTerminal = 501,
  PartialTransition = 502,
  EffectNotObserved = 503,
  AcknowledgementIsNotProof = 504,
  DuplicateCommand = 505,
  IdempotencyConflict = 506,
  CommandRefused = 507,
  AdapterUnavailable = 508,
  DegradedOperation = 509,
  AttemptInterrupted = 510,
  ObservationUnknownCommand = 512,

  // 6xx - recovery and rebalance.
  HysteresisNotSatisfied = 600,
  RecoveryNotEligible = 601,
  IncumbentHealthy = 602,
  OscillationGuard = 603,

  // 7xx - durability.
  StoreNotFound = 700,
  StoreCorrupt = 701,
  StoreTruncated = 702,
  StoreVersionUnsupported = 703,
  StoreLocked = 704,
  StoreIoError = 705,
  StoreAlreadyOpen = 706,
  StoreClosed = 707,
  StoreTrailingBytes = 708,
  StoreRolledBack = 709,
  StoreLimitExceeded = 710,
  StoreRecoveredStale = 711,
  StoreReservedFieldSet = 712,

  // 8xx - arithmetic and capacity.
  Overflow = 800,
  LimitExceeded = 801,
  CapacityExceeded = 802,

  // 9xx - internal.
  InternalError = 900,
  NotImplemented = 901,
  Unsupported = 902,
};

/// Symbolic name of an error code. Stable and suitable for logs and tests.
[[nodiscard]] CF_API const char* to_string(ErrorCode code) noexcept;

/// Classification of an error code. Used for exit codes and retry policy.
enum class ErrorClass : std::uint8_t {
  None = 0,
  Invalid = 1,       ///< Caller supplied something structurally wrong.
  Stale = 2,         ///< Caller is bound to superseded state.
  Denied = 3,        ///< Authority explicitly refused.
  Indeterminate = 4, ///< Evidence exists but is not decisive.
  Conflict = 5,      ///< Current state forbids the operation.
  Unavailable = 6,   ///< A dependency is missing or unreachable.
  Internal = 7,
  Unsupported = 8,   ///< Understood but not supported by this build or peer.
};

[[nodiscard]] CF_API ErrorClass classify(ErrorCode code) noexcept;

namespace detail {
[[nodiscard]] CF_API std::string to_context_string(std::string_view value);
[[nodiscard]] CF_API std::string to_context_string(const std::string& value);
[[nodiscard]] CF_API std::string to_context_string(const char* value);
[[nodiscard]] CF_API std::string to_context_string(bool value);
[[nodiscard]] CF_API std::string to_context_string(std::uint64_t value);
[[nodiscard]] CF_API std::string to_context_string(std::int64_t value);
[[nodiscard]] CF_API std::string to_context_string(std::uint32_t value);
[[nodiscard]] CF_API std::string to_context_string(std::int32_t value);
}  // namespace detail

/// A status: stable code plus human explanation plus structured context.
///
/// Context entries are kept sorted by key so that two statuses produced by
/// equivalent failures compare equal and encode identically.
class CF_API Status {
 public:
  Status() = default;

  /// The success status.
  [[nodiscard]] static Status success() noexcept { return Status{}; }

  [[nodiscard]] static Status error(ErrorCode code, std::string message) {
    Status status;
    status.code_ = code;
    status.message_ = std::move(message);
    return status;
  }

  /// Error with one or more key/value context pairs.
  template <class T, class... Rest>
  [[nodiscard]] static Status error(ErrorCode code, std::string message, std::string key, T&& value,
                                    Rest&&... rest) {
    Status status = error(code, std::move(message));
    status.append_context(std::move(key), std::forward<T>(value), std::forward<Rest>(rest)...);
    return status;
  }

  template <class T>
  Status& with(std::string key, T&& value) {
    set_context(std::move(key), detail::to_context_string(std::forward<T>(value)));
    return *this;
  }

  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::Ok; }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }
  [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& context() const noexcept {
    return context_;
  }
  [[nodiscard]] ErrorClass error_class() const noexcept { return classify(code_); }

  [[nodiscard]] std::string to_string() const;

  /// Deterministic single-line rendering; used by the CLI and by tests.
  [[nodiscard]] std::string canonical_line() const;

  friend bool operator==(const Status& lhs, const Status& rhs) {
    return lhs.code_ == rhs.code_ && lhs.message_ == rhs.message_ && lhs.context_ == rhs.context_;
  }
  friend bool operator!=(const Status& lhs, const Status& rhs) { return !(lhs == rhs); }

 private:
  void set_context(std::string key, std::string value);

  void append_context() {}
  template <class T, class... Rest>
  void append_context(std::string key, T&& value, Rest&&... rest) {
    set_context(std::move(key), detail::to_context_string(std::forward<T>(value)));
    append_context(std::forward<Rest>(rest)...);
  }

  ErrorCode code_{ErrorCode::Ok};
  std::string message_{};
  std::vector<std::pair<std::string, std::string>> context_{};
};

/// Result of an operation: either a value or a non-Ok Status.
///
/// \note Result<T> may not be instantiated with Status; use Result<void>.
template <class T>
class [[nodiscard]] Result {
  static_assert(!std::is_same_v<T, Status>, "Result<Status> is meaningless; use Result<void>");

 public:
  Result(T value) : value_(std::move(value)) {}         // NOLINT(google-explicit-constructor)
  Result(Status status) : status_(std::move(status)) {  // NOLINT(google-explicit-constructor)
    if (status_.ok()) {
      status_ = Status::error(ErrorCode::InternalError,
                              "Result constructed from an Ok status without a value");
    }
  }

  [[nodiscard]] bool has_value() const noexcept { return value_.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] const Status& status() const noexcept { return status_; }

  [[nodiscard]] T& value() & { return *value_; }
  [[nodiscard]] const T& value() const& { return *value_; }
  [[nodiscard]] T&& value() && { return std::move(*value_); }
  [[nodiscard]] T& operator*() & { return *value_; }
  [[nodiscard]] const T& operator*() const& { return *value_; }
  [[nodiscard]] T&& operator*() && { return std::move(*value_); }
  [[nodiscard]] T* operator->() { return &*value_; }
  [[nodiscard]] const T* operator->() const { return &*value_; }

 private:
  std::optional<T> value_{};
  Status status_{};
};

template <>
class [[nodiscard]] Result<void> {
 public:
  Result() = default;
  Result(Status status) : status_(std::move(status)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool has_value() const noexcept { return status_.ok(); }
  [[nodiscard]] explicit operator bool() const noexcept { return status_.ok(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }

 private:
  Status status_{};
};

}  // namespace cooling_failover

/// Binds the value of a successful Result to a new local, or returns its
/// failure status from the current function.
#define CF_TRY_ASSIGN(var, expr)          \
  auto cf_try_tmp_##var = (expr);         \
  if (!cf_try_tmp_##var.has_value()) {    \
    return cf_try_tmp_##var.status();     \
  }                                       \
  auto&& var = *cf_try_tmp_##var

#define CF_TRY(expr)                    \
  do {                                  \
    auto&& cf_try_tmp = (expr);         \
    if (!cf_try_tmp.has_value()) {      \
      return cf_try_tmp.status();       \
    }                                   \
  } while (false)
