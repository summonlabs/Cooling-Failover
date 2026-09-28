// Cooling Failover - plans, transition attempts, commands and observations.
#pragma once

#include "cooling_failover/canonical.hpp"
#include "cooling_failover/eligibility.hpp"
#include "cooling_failover/export.hpp"
#include "cooling_failover/ids.hpp"
#include "cooling_failover/model.hpp"
#include "cooling_failover/status.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cooling_failover {

/// Caller-supplied request identity, used to make public mutations idempotent.
struct IdempotencyKeyTag;
using IdempotencyKey = Id<IdempotencyKeyTag>;

/// What the plan is trying to achieve.
enum class PlanKind : std::uint8_t {
  Failover = 1,         ///< move a protected domain onto an alternate arrangement
  Rebalance = 2,        ///< move onto a different arrangement while service holds
  ReturnToPrimary = 3,  ///< return service to the designated primary arrangement
};

/// Effect that must be observed before a step counts as transferred.
enum class EffectKind : std::uint8_t {
  FlowEstablished = 1,
  CapacityDelivered = 2,
  ReturnTemperatureInBand = 3,
  IncumbentIsolated = 4,
  RedundancyRestored = 5,
};

[[nodiscard]] CF_API const char* to_string(EffectKind effect) noexcept;
[[nodiscard]] CF_API const char* to_string(PlanKind kind) noexcept;

/// Where an effect observation came from. Acknowledgement origins are recorded
/// for audit but never satisfy a required effect.
enum class ObservationOrigin : std::uint8_t {
  AirflowControl = 1,
  LiquidCoolingControl = 2,
  CoolingCapacity = 3,
  CoolingTopology = 4,
  FacilityBms = 5,
  ActuationControllerAck = 6,
};

[[nodiscard]] CF_API const char* to_string(ObservationOrigin origin) noexcept;
[[nodiscard]] CF_API bool origin_can_verify(ObservationOrigin origin) noexcept;

enum class PlanState : std::uint8_t {
  Active = 1,
  Completed = 2,
  Fenced = 3,
  Failed = 4,
  Superseded = 5,
};

[[nodiscard]] CF_API const char* to_string(PlanState state) noexcept;

/// Why a plan was fenced.
enum class FenceReason : std::uint8_t {
  None = 0,
  EpochSuperseded = 1,
  TopologyGenerationChanged = 2,
  CapacityGenerationChanged = 3,
  PolicyGenerationChanged = 4,
  EvidenceChanged = 5,
  AuthorityLost = 6,
  Manual = 7,
  RequestedByCaller = 8,
};

[[nodiscard]] CF_API const char* to_string(FenceReason reason) noexcept;

enum class AttemptState : std::uint8_t {
  Created = 1,
  RequestsIssued = 2,
  Acknowledged = 3,        ///< controller said yes; not proof of transfer
  PartiallyObserved = 4,   ///< some required effects observed; NOT completion
  Verified = 5,            ///< every required effect observed from an owning system
  Failed = 6,
  Fenced = 7,
  Interrupted = 8,         ///< recovered after restart; needs re-verification
  Refused = 9,
};

[[nodiscard]] CF_API const char* to_string(AttemptState state) noexcept;
[[nodiscard]] CF_API bool attempt_state_is_terminal(AttemptState state) noexcept;

enum class CommandOutcome : std::uint8_t {
  NotIssued = 1,
  Acknowledged = 2,
  Refused = 3,
  RuntimeUnavailable = 4,
  Replayed = 5,  ///< matched a durable prior issue; nothing was re-actuated
  Rejected = 6,  ///< fenced or invalid before reaching the runtime
};

[[nodiscard]] CF_API const char* to_string(CommandOutcome outcome) noexcept;

struct CF_API PlanStep {
  std::uint32_t ordinal{0};
  SourceGroupId target{};
  std::vector<EffectKind> required_effects{};  ///< ascending

  friend bool operator==(const PlanStep&, const PlanStep&) = default;
};

struct CF_API CommandSpec {
  CommandId id{};
  IdempotencyKey key{};
  Digest fingerprint{};
  std::uint32_t step{0};
  OwnerSystem runtime{OwnerSystem::AirflowControl};
  SourceGroupId target{};
  std::vector<EffectKind> required_effects{};  ///< ascending

  friend bool operator==(const CommandSpec&, const CommandSpec&) = default;
};

/// A plan bound to the exact generations it was created against. Any change to
/// a bound generation fences the plan.
struct CF_API FailoverPlan {
  PlanId id{};
  /// Caller request identity this plan was created for, and the fingerprint of
  /// that request. Both are durable so that a lost-response retry replays the
  /// original plan instead of creating a second one.
  RequestId request{};
  Digest request_fingerprint{};
  FailoverDomainId domain{};
  PlanKind kind{PlanKind::Failover};
  ControlPlaneEpoch epoch{};
  StateRevision revision_at_creation{};
  TopologyGeneration topology{};
  CapacityGeneration capacity{};
  PolicyGeneration policy{};
  EvidenceSetGeneration evidence_generation{};
  Digest evidence_digest{};
  std::uint64_t obligations_generation{0};
  SourceGroupId incumbent{};
  SourceGroupId target{};
  std::vector<SourceGroupId> target_groups{};
  std::vector<PlanStep> steps{};
  std::vector<CommandSpec> commands{};
  Tick created_at{};
  PlanState state{PlanState::Active};
  FenceReason fence_reason{FenceReason::None};
  Tick fenced_at{};
  std::uint32_t attempts_started{0};
  std::string reason{};

  /// Digest binding every field that must not change while the plan runs.
  [[nodiscard]] Digest binding_digest() const;
};

struct CF_API CommandRecord {
  CommandId id{};
  IdempotencyKey key{};
  Digest fingerprint{};
  PlanId plan{};
  AttemptId attempt{};
  std::uint32_t step{0};
  OwnerSystem runtime{OwnerSystem::AirflowControl};
  SourceGroupId target{};
  CommandOutcome outcome{CommandOutcome::NotIssued};
  IssueSequence issue_sequence{};
  Tick issued_at{};
  ControlPlaneEpoch epoch{};
  std::string detail{};
};

/// Durable idempotency record: the fingerprint a key was first used with and the
/// result that must be replayed for a lost-response retry.
struct CF_API IdempotencyRecord {
  IdempotencyKey key{};
  Digest fingerprint{};
  CommandOutcome outcome{CommandOutcome::NotIssued};
  CommandId command{};
  std::string detail{};
};

struct CF_API EffectObservationRecord {
  ObservationId id{};
  CommandId command{};
  SourceGroupId group{};
  EffectKind effect{EffectKind::FlowEstablished};
  ObservationOrigin origin{ObservationOrigin::AirflowControl};
  EffectSequence sequence{};
  TopologyGeneration topology{};
  CapacityGeneration capacity{};
  ControlPlaneEpoch epoch{};
  Tick observed_at{};
  /// Set when the record was loaded from durable state. Recovered observations
  /// are not current physical evidence until an owning system re-sends them.
  bool recovered_unvalidated{false};
};

struct CF_API TransitionAttempt {
  AttemptId id{};
  PlanId plan{};
  std::uint32_t number{0};
  ControlPlaneEpoch epoch{};
  AttemptState state{AttemptState::Created};
  Tick created_at{};
  Tick updated_at{};
  StateRevision revision_at_issue{};
  Digest plan_binding{};
  std::vector<CommandId> commands{};
  std::vector<EffectKind> verified_effects{};  ///< ascending
  std::vector<EffectKind> missing_effects{};   ///< ascending
  std::uint32_t commands_issued{0};
  std::uint32_t commands_replayed{0};
  std::uint32_t commands_refused{0};
  std::uint32_t observations_accepted{0};
  std::string detail{};
};

/// One recorded transition in the domain history, used for oscillation control.
struct CF_API TransitionRecord {
  PlanId plan{};
  PlanKind kind{PlanKind::Failover};
  SourceGroupId from_group{};
  SourceGroupId to_group{};
  Tick at{};
};

/// Rolling recovery evidence for one source group.
struct CF_API HealthStreak {
  SourceGroupId group{};
  std::uint32_t consecutive_healthy{0};
  Tick streak_started_at{};
  Tick last_observed_at{};
  TopologyGeneration topology{};
  CapacityGeneration capacity{};
};

}  // namespace cooling_failover
