// Cooling Failover - strongly typed identities, generations, epochs and counters.
//
// Materially different concepts are distinct C++ types. An epoch cannot be
// passed where a topology generation is expected, and vice versa.
#pragma once

#include "cooling_failover/export.hpp"
#include "cooling_failover/status.hpp"

#include <compare>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace cooling_failover {

namespace detail {

/// Canonical rendering of a 64-bit identity: "0x" plus 16 lower-case hex digits.
[[nodiscard]] CF_API std::string hex64(std::uint64_t value);
/// Parses "0x<hex>" (1..16 digits) or a plain decimal literal.
[[nodiscard]] CF_API Result<std::uint64_t> parse_u64(std::string_view text, std::string_view field);

}  // namespace detail

/// Identity of an object owned by this orchestrator or referenced from it.
///
/// The value is opaque; only equality and ordering are meaningful. Value 0 is
/// reserved and means "absent"; APIs that require an identity reject it.
template <class Tag>
class [[nodiscard]] Id {
 public:
  using value_type = std::uint64_t;

  constexpr Id() noexcept = default;
  explicit constexpr Id(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr Id from_value(std::uint64_t value) noexcept { return Id(value); }
  [[nodiscard]] static constexpr Id nil() noexcept { return Id(0); }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_nil() const noexcept { return value_ == 0; }

  [[nodiscard]] std::string to_string() const { return detail::hex64(value_); }

  [[nodiscard]] static Result<Id> parse(std::string_view text, std::string_view field) {
    Result<std::uint64_t> parsed = detail::parse_u64(text, field);
    if (!parsed.has_value()) {
      return parsed.status();
    }
    return Id(*parsed);
  }

  friend constexpr auto operator<=>(const Id&, const Id&) noexcept = default;

 private:
  std::uint64_t value_{0};
};

/// Monotonic version of state owned elsewhere (topology, capacity accounting,
/// policy, imported evidence) or of an authority epoch.
///
/// Value 0 means "unset": the orchestrator has never observed this generation.
template <class Tag>
class [[nodiscard]] Generation {
 public:
  using value_type = std::uint64_t;

  constexpr Generation() noexcept = default;
  explicit constexpr Generation(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr Generation from_value(std::uint64_t value) noexcept {
    return Generation(value);
  }
  [[nodiscard]] static constexpr Generation unset() noexcept { return Generation(0); }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_set() const noexcept { return value_ != 0; }

  /// Checked successor. Fails with Overflow at the numeric maximum.
  [[nodiscard]] Result<Generation> next() const {
    if (value_ == std::numeric_limits<std::uint64_t>::max()) {
      return Status::error(ErrorCode::Overflow, "generation exhausted");
    }
    return Generation(value_ + 1);
  }

  [[nodiscard]] std::string to_string() const { return detail::hex64(value_); }

  [[nodiscard]] static Result<Generation> parse(std::string_view text, std::string_view field) {
    Result<std::uint64_t> parsed = detail::parse_u64(text, field);
    if (!parsed.has_value()) {
      return parsed.status();
    }
    return Generation(*parsed);
  }

  friend constexpr auto operator<=>(const Generation&, const Generation&) noexcept = default;

 private:
  std::uint64_t value_{0};
};

/// Monotonic sequence number: revisions, commit sequences, effect sequences.
/// Distinct from Generation because sequences advance locally by one per event
/// while generations are assigned by an owning system.
template <class Tag>
class [[nodiscard]] Counter {
 public:
  using value_type = std::uint64_t;

  constexpr Counter() noexcept = default;
  explicit constexpr Counter(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr Counter from_value(std::uint64_t value) noexcept {
    return Counter(value);
  }
  [[nodiscard]] static constexpr Counter initial() noexcept { return Counter(0); }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

  [[nodiscard]] Result<Counter> next() const { return advanced_by(1); }

  [[nodiscard]] Result<Counter> advanced_by(std::uint64_t delta) const {
    if (delta > std::numeric_limits<std::uint64_t>::max() - value_) {
      return Status::error(ErrorCode::Overflow, "counter exhausted");
    }
    return Counter(value_ + delta);
  }

  [[nodiscard]] bool is_before(const Counter& other) const noexcept { return value_ < other.value_; }

  friend constexpr auto operator<=>(const Counter&, const Counter&) noexcept = default;

 private:
  std::uint64_t value_{0};
};

/// Orchestrator logical time. Never the wall clock: policy windows and
/// hysteresis are evaluated against caller-supplied logical ticks so that
/// behaviour is reproducible.
class [[nodiscard]] Tick {
 public:
  constexpr Tick() noexcept = default;
  explicit constexpr Tick(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr Tick from_value(std::uint64_t value) noexcept { return Tick(value); }
  [[nodiscard]] static constexpr Tick epoch_start() noexcept { return Tick(0); }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

  [[nodiscard]] Result<Tick> advanced_by(std::uint64_t delta) const {
    if (delta > std::numeric_limits<std::uint64_t>::max() - value_) {
      return Status::error(ErrorCode::Overflow, "tick exhausted");
    }
    return Tick(value_ + delta);
  }

  /// Saturating elapsed ticks; returns 0 when \p earlier is in the future.
  [[nodiscard]] std::uint64_t elapsed_since(Tick earlier) const noexcept {
    return value_ >= earlier.value_ ? value_ - earlier.value_ : 0;
  }

  [[nodiscard]] static Result<Tick> parse(std::string_view text, std::string_view field) {
    Result<std::uint64_t> parsed = detail::parse_u64(text, field);
    if (!parsed.has_value()) {
      return parsed.status();
    }
    return Tick(*parsed);
  }

  friend constexpr auto operator<=>(const Tick&, const Tick&) noexcept = default;

 private:
  std::uint64_t value_{0};
};

// ---------------------------------------------------------------------------
// Domain identities.
// ---------------------------------------------------------------------------

struct FailoverDomainTag;
struct SourceGroupTag;
struct CoolingSourceTag;
struct ReserveBlockTag;
struct ObligationTag;
struct PlanTag;
struct AttemptTag;
struct CommandTag;
struct ObservationTag;
struct EvidenceTag;
struct CandidateTag;
struct RequestTag;

/// A protected cooling domain: the unit of failover authority. At most one
/// failover plan may be active for a domain at a time.
using FailoverDomainId = Id<FailoverDomainTag>;
/// A structurally independent arrangement of cooling sources.
using SourceGroupId = Id<SourceGroupTag>;
/// A single cooling source (chiller, CDU, pump set, CRAH group, ...).
using CoolingSourceId = Id<CoolingSourceTag>;
/// A unit of reserve capacity that may only be accounted once.
using ReserveBlockId = Id<ReserveBlockTag>;
/// A protected obligation (a load that must keep being cooled).
using ObligationId = Id<ObligationTag>;
using PlanId = Id<PlanTag>;
using AttemptId = Id<AttemptTag>;
using CommandId = Id<CommandTag>;
using ObservationId = Id<ObservationTag>;
using EvidenceId = Id<EvidenceTag>;
using CandidateId = Id<CandidateTag>;
/// Caller-supplied request identity used for idempotent public operations.
using RequestId = Id<RequestTag>;

// ---------------------------------------------------------------------------
// Generations owned by other systems, imported as evidence.
// ---------------------------------------------------------------------------

struct TopologyGenerationTag;
struct CapacityGenerationTag;
struct PolicyGenerationTag;
struct EvidenceSetGenerationTag;
struct ConfigGenerationTag;
struct ControlPlaneEpochTag;

/// Generation of the cooling topology projection owned by the topology system.
using TopologyGeneration = Generation<TopologyGenerationTag>;
/// Generation of cooling-capacity accounting owned by the capacity system.
using CapacityGeneration = Generation<CapacityGenerationTag>;
/// Generation of the redundancy policy installed for a domain.
using PolicyGeneration = Generation<PolicyGenerationTag>;
/// Generation of the set of evidence the orchestrator currently holds.
using EvidenceSetGeneration = Generation<EvidenceSetGenerationTag>;
/// Generation of local orchestrator configuration.
using ConfigGeneration = Generation<ConfigGenerationTag>;
/// Control-plane epoch. Advancing the epoch supersedes all prior authority.
using ControlPlaneEpoch = Generation<ControlPlaneEpochTag>;

// ---------------------------------------------------------------------------
// Local counters.
// ---------------------------------------------------------------------------

struct StateRevisionTag;
struct CommitSequenceTag;
struct EffectSequenceTag;
struct IssueSequenceTag;

/// Revision of the in-memory domain state; bumps on every accepted mutation.
using StateRevision = Counter<StateRevisionTag>;
/// Durable commit sequence; the durable commit point of the store.
using CommitSequence = Counter<CommitSequenceTag>;
/// Effect-sequence space owned by the controllers that observe cooling effects.
using EffectSequence = Counter<EffectSequenceTag>;
/// Command issue sequence assigned by this orchestrator.
using IssueSequence = Counter<IssueSequenceTag>;

}  // namespace cooling_failover
