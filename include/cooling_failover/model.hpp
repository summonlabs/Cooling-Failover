// Cooling Failover - imported domain model.
//
// Everything in this header describes state OWNED BY ANOTHER SYSTEM that this
// orchestrator consumes as generation-stamped evidence: cooling topology,
// cooling-capacity accounting, source health, service authority, redundancy
// policy and protected obligations. Nothing here re-derives topology or
// capacity; the orchestrator only validates, binds and reasons over it.
#pragma once

#include "cooling_failover/canonical.hpp"
#include "cooling_failover/export.hpp"
#include "cooling_failover/ids.hpp"
#include "cooling_failover/status.hpp"
#include "cooling_failover/units.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cooling_failover {

/// Systems that own state this orchestrator consumes or requests work from.
enum class OwnerSystem : std::uint8_t {
  CoolingTopology = 1,       ///< owns the cooling topology graph
  CoolingCapacity = 2,       ///< owns cooling-capacity accounting
  AirflowControl = 3,        ///< owns air-side actuation
  LiquidCoolingControl = 4,  ///< owns liquid-side actuation
  FacilityBms = 5,           ///< owns facility-level sequencing
  PowerControl = 6,          ///< owns power failover (never orchestrated here)
  ThermalZone = 7,           ///< owns thermal-zone modelling
  CoolingFailover = 8,       ///< this orchestrator (never a source of physical evidence)
  ActuationControllerAck = 9 ///< acknowledgement only; never proof of transfer
};

/// Acknowledge-only origins can never verify a cooling transfer.
[[nodiscard]] CF_API bool is_evidence_bearing_origin(OwnerSystem owner) noexcept;

enum class EvidenceKind : std::uint8_t {
  TopologyProjection = 1,
  CapacityAvailability = 2,
  SourceHealth = 3,
  ServiceAuthority = 4,
  RedundancyPolicy = 5,
  ProtectedObligationSet = 6,
  CommandAcknowledgement = 7,  ///< not accepted as transfer proof
  EffectObservation = 8,
};

/// Every imported item carries a stamp identifying its owner and generation.
struct CF_API EvidenceStamp {
  EvidenceId evidence{};
  EvidenceKind kind{EvidenceKind::TopologyProjection};
  OwnerSystem owner{OwnerSystem::CoolingTopology};
  Tick observed_at{};
  std::string reference{};  ///< opaque owner-side reference, bounded length

  friend bool operator==(const EvidenceStamp&, const EvidenceStamp&) = default;
};

/// Physical class of a cooling source.
enum class SourceKind : std::uint8_t {
  Chiller = 1,
  Cdu = 2,
  Crac = 3,
  Crah = 4,
  PrimaryPump = 5,
  SecondaryLoop = 6,
  Economizer = 7,
  ThermalStore = 8,
};

/// Air-side effectors are actuated through Airflow Control, everything else
/// through Liquid Cooling Control. This is the only place the mapping exists.
[[nodiscard]] CF_API OwnerSystem actuation_runtime_for(SourceKind kind) noexcept;

/// Health as reported by the owning system. Absent evidence is not Healthy.
enum class HealthState : std::uint8_t {
  Unknown = 1,
  Healthy = 2,
  Degraded = 3,
  Unavailable = 4,
  Unsupported = 5,
};

[[nodiscard]] CF_API const char* to_string(HealthState state) noexcept;
[[nodiscard]] CF_API const char* to_string(OwnerSystem owner) noexcept;
[[nodiscard]] CF_API const char* to_string(EvidenceKind kind) noexcept;
[[nodiscard]] CF_API const char* to_string(SourceKind kind) noexcept;

/// Service authority for a failover domain, as granted by the authority owner.
enum class AuthorityState : std::uint8_t {
  Unknown = 1,
  Granted = 2,
  Denied = 3,
  Superseded = 4,
};

[[nodiscard]] CF_API const char* to_string(AuthorityState state) noexcept;

/// Criticality of a protected obligation.
enum class Criticality : std::uint8_t {
  Critical = 1,
  Important = 2,
  BestEffort = 3,
};

/// Availability of a unit of reserve capacity. Absent, unknown, unsupported,
/// indeterminate and zero are separate states and are never conflated.
enum class AvailabilityState : std::uint8_t {
  Available = 1,      ///< owner reports usable capacity greater than zero
  Zero = 2,           ///< owner reports exactly zero usable capacity
  Unavailable = 3,    ///< owner reports the block out of service
  Unknown = 4,        ///< owner cannot currently determine
  Unsupported = 5,    ///< owner does not measure this
  Indeterminate = 6,  ///< owner reports a value it does not trust
};

[[nodiscard]] CF_API const char* to_string(AvailabilityState state) noexcept;

// ---------------------------------------------------------------------------
// Topology projection (owned by the cooling topology system).
// ---------------------------------------------------------------------------

struct CF_API SourceDescriptor {
  CoolingSourceId source{};
  SourceGroupId group{};
  SourceKind kind{SourceKind::Chiller};
  Watts nominal_capacity{};

  friend bool operator==(const SourceDescriptor&, const SourceDescriptor&) = default;
};

struct CF_API SourceGroupDescriptor {
  SourceGroupId group{};
  Watts nominal_capacity{};
  bool designated_primary{false};  ///< structural claim only; never eligibility

  friend bool operator==(const SourceGroupDescriptor&, const SourceGroupDescriptor&) = default;
};

/// A unit of reserve capacity that one or more source groups can draw on.
/// A block that serves several groups is exactly how double counting arises,
/// so the ledger allocates blocks, never group totals.
struct CF_API ReserveBlockDescriptor {
  ReserveBlockId block{};
  SourceGroupId owner_group{};                ///< group that physically contains it
  CoolingSourceId owner_source{};             ///< source that provides it
  Watts nominal_capacity{};
  std::vector<SourceGroupId> serving_groups{};  ///< ascending, includes owner_group

  friend bool operator==(const ReserveBlockDescriptor&, const ReserveBlockDescriptor&) = default;
};

/// Imported topology. Members, sources, groups and blocks are required to be in
/// strictly ascending identity order with no duplicates.
struct CF_API TopologyProjection {
  EvidenceStamp stamp{};
  FailoverDomainId domain{};
  TopologyGeneration generation{};
  std::vector<SourceGroupDescriptor> groups{};
  std::vector<SourceDescriptor> sources{};
  std::vector<ReserveBlockDescriptor> blocks{};

  [[nodiscard]] const SourceGroupDescriptor* find_group(SourceGroupId group) const noexcept;
  [[nodiscard]] bool contains_group(SourceGroupId group) const noexcept;
  [[nodiscard]] std::vector<CoolingSourceId> members_of(SourceGroupId group) const;
  [[nodiscard]] std::vector<ReserveBlockId> blocks_serving(SourceGroupId group) const;
};

// ---------------------------------------------------------------------------
// Capacity availability (owned by the cooling-capacity system).
// ---------------------------------------------------------------------------

struct CF_API BlockAvailability {
  ReserveBlockId block{};
  AvailabilityState state{AvailabilityState::Unknown};
  Watts available{};
  Tick observed_at{};

  friend bool operator==(const BlockAvailability&, const BlockAvailability&) = default;
};

/// One complete generation-stamped snapshot of reserve-block availability for a
/// domain. A block that is absent from the snapshot is Unknown, never zero.
struct CF_API CapacityAvailabilityEvidence {
  EvidenceStamp stamp{};
  FailoverDomainId domain{};
  CapacityGeneration generation{};
  std::vector<BlockAvailability> blocks{};  ///< ascending by block id

  [[nodiscard]] const BlockAvailability* find(ReserveBlockId block) const noexcept;
};

// ---------------------------------------------------------------------------
// Source health (owned by the control runtimes / BMS).
// ---------------------------------------------------------------------------

struct CF_API SourceHealthEvidence {
  EvidenceStamp stamp{};
  CoolingSourceId source{};
  HealthState state{HealthState::Unknown};

  friend bool operator==(const SourceHealthEvidence&, const SourceHealthEvidence&) = default;
};

// ---------------------------------------------------------------------------
// Service authority (owned by the DCCP authority owner).
// ---------------------------------------------------------------------------

struct CF_API ServiceAuthorityEvidence {
  EvidenceStamp stamp{};
  FailoverDomainId domain{};
  ControlPlaneEpoch epoch{};
  AuthorityState state{AuthorityState::Unknown};

  friend bool operator==(const ServiceAuthorityEvidence&, const ServiceAuthorityEvidence&) = default;
};

// ---------------------------------------------------------------------------
// Redundancy policy (owned at the orchestration boundary, generation-stamped).
// ---------------------------------------------------------------------------

/// How a candidate is chosen. The orchestrator selects a candidate only when the
/// policy explicitly defines a deterministic ordering.
enum class SelectionMode : std::uint8_t {
  NotDefined = 1,         ///< selection is refused with SelectionNotDefined
  DeterministicRanked = 2,///< rank by rank_order, ties broken by candidate key
  ManualOnly = 3,         ///< a target must be named by the caller
};

enum class RankCriterion : std::uint8_t {
  AscendingGroupKey = 1,        ///< lexicographic over ascending group ids
  DescendingUsableCapacity = 2, ///< more usable capacity first
  DescendingHeadroom = 3,       ///< more surplus above obligations first
  FewestGroups = 4,             ///< simpler arrangements first
  MostGroups = 5,               ///< more independent groups first
  OwnerGroupFirst = 6,          ///< prefer blocks owned by the arrangement's groups
};

[[nodiscard]] CF_API const char* to_string(RankCriterion criterion) noexcept;
[[nodiscard]] CF_API const char* to_string(SelectionMode mode) noexcept;

struct CF_API RedundancyPolicy {
  EvidenceStamp stamp{};
  PolicyGeneration generation{};
  SelectionMode selection{SelectionMode::NotDefined};
  std::vector<RankCriterion> rank_order{};
  std::uint32_t minimum_independent_groups{2};
  std::uint32_t max_groups_per_candidate{1};
  BasisPoints headroom_margin{};
  std::uint32_t recovery_consecutive_healthy{3};
  std::uint64_t recovery_dwell_ticks{30};
  std::uint64_t max_evidence_age_ticks{0};  ///< 0 disables the age limit
  std::uint64_t oscillation_window_ticks{60};
  std::uint32_t max_transitions_per_window{3};
  std::uint32_t max_plan_steps{16};

  friend bool operator==(const RedundancyPolicy&, const RedundancyPolicy&) = default;
};

/// Upper bounds applied to policy input so that no allocation is driven by an
/// untrusted declared size.
inline constexpr std::uint32_t kMaxRankCriteria = 8;
inline constexpr std::uint32_t kMaxGroupsPerCandidate = 8;
inline constexpr std::uint32_t kMaxGroupsPerDomain = 16;
inline constexpr std::uint32_t kMaxSourcesPerDomain = 256;
inline constexpr std::uint32_t kMaxBlocksPerDomain = 256;
inline constexpr std::uint32_t kMaxObligationsPerDomain = 64;
inline constexpr std::uint32_t kMaxPlanSteps = 64;
inline constexpr std::uint32_t kMaxCandidateSubsets = 65536;
inline constexpr std::uint32_t kMaxRetainedObservations = 4096;
inline constexpr std::uint32_t kMaxTransitionHistory = 256;
inline constexpr std::uint32_t kMaxRetainedPlans = 256;
inline constexpr std::uint32_t kMaxRetainedAttempts = 512;
inline constexpr std::uint32_t kMaxRetainedCommands = 4096;
inline constexpr std::uint64_t kMaxEvidenceAgeTicks = 1000000000ULL;

// ---------------------------------------------------------------------------
// Protected obligations (owned at the orchestration boundary).
// ---------------------------------------------------------------------------

struct CF_API ProtectedObligation {
  ObligationId id{};
  Watts required_capacity{};
  std::uint32_t required_independent_groups{1};
  Criticality criticality{Criticality::Critical};
  std::string reference{};

  friend bool operator==(const ProtectedObligation&, const ProtectedObligation&) = default;
};

struct CF_API ObligationSet {
  EvidenceStamp stamp{};
  std::uint64_t generation{0};
  std::vector<ProtectedObligation> obligations{};  ///< ascending by obligation id

  [[nodiscard]] std::uint64_t total_required_capacity() const noexcept;
};

}  // namespace cooling_failover
