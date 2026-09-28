// Cooling Failover - durable domain state and its event codec.
//
// The durable store is an append-only log of bounded events. Exactly one record
// is committed per accepted mutation, so a crash resolves to either the
// previous complete generation or the new one and never to a hybrid.
#pragma once

#include "cooling_failover/canonical.hpp"
#include "cooling_failover/eligibility.hpp"
#include "cooling_failover/export.hpp"
#include "cooling_failover/model.hpp"
#include "cooling_failover/plan.hpp"
#include "cooling_failover/status.hpp"
#include "cooling_failover/store.hpp"

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace cooling_failover {

/// Complete orchestration state of one failover domain.
///
/// Every container is ordered by identity, so two states built by the same
/// sequence of accepted mutations encode to identical bytes.
struct CF_API DomainState {
  FailoverDomainId domain{};
  StateRevision revision{};
  ControlPlaneEpoch epoch{};
  bool epoch_established{false};
  Tick registered_at{};
  bool registered{false};

  bool has_topology{false};
  TopologyProjection topology{};
  bool has_capacity{false};
  CapacityAvailabilityEvidence capacity{};
  bool has_policy{false};
  RedundancyPolicy policy{};
  bool has_obligations{false};
  ObligationSet obligations{};
  bool has_authority{false};
  ServiceAuthorityEvidence authority{};

  EvidenceSetGeneration evidence_generation{};
  Digest evidence_digest{};

  std::map<CoolingSourceId, SourceHealthEvidence> health{};
  std::map<PlanId, FailoverPlan> plans{};
  std::map<AttemptId, TransitionAttempt> attempts{};
  std::map<CommandId, CommandRecord> commands{};
  std::map<IdempotencyKey, IdempotencyRecord> idempotency{};
  std::map<RequestId, std::pair<Digest, PlanId>> plan_requests{};
  std::map<ObservationId, EffectObservationRecord> observations{};
  std::vector<ObservationId> observation_order{};
  std::map<SourceGroupId, HealthStreak> streaks{};
  std::vector<TransitionRecord> transitions{};

  PlanId active_plan{};
  AttemptId active_attempt{};
  CommitSequence last_commit{};
  Tick last_tick{};
  bool recovered{false};

  /// Bumps the revision and the evidence-set generation after an accepted
  /// mutation that changed imported evidence.
  void touch_evidence(Tick now);

  [[nodiscard]] Result<void> validate() const;
};

/// Applies one durable event. When \p recovering is true the event came from
/// durable storage: dynamic observations are marked recovered-unvalidated,
/// health streaks are discarded, and non-terminal attempts are interrupted.
[[nodiscard]] CF_API Result<void> apply_event(DomainState& state, RecordType type,
                                              std::span<const std::uint8_t> payload,
                                              bool recovering);

/// Encodes the complete state as a compacted event stream that rebuilds it from
/// an empty state. Used for snapshot publication.
[[nodiscard]] CF_API std::vector<std::uint8_t> encode_snapshot_events(const DomainState& state);

/// Decodes an event stream produced by encode_snapshot_events.
[[nodiscard]] CF_API Result<std::vector<std::pair<RecordType, std::vector<std::uint8_t>>>>
decode_event_stream(std::span<const std::uint8_t> bytes);

/// Canonical byte encoding of the whole state, used for determinism proofs.
[[nodiscard]] CF_API std::vector<std::uint8_t> encode_state_canonical(const DomainState& state);

// Individual event payload codecs. Exposed so that tests can build and corrupt
// events directly.
[[nodiscard]] CF_API std::vector<std::uint8_t> encode_domain_registered(Tick now);
[[nodiscard]] CF_API std::vector<std::uint8_t> encode_epoch_established(ControlPlaneEpoch epoch,
                                                                       Tick now);
[[nodiscard]] CF_API std::vector<std::uint8_t> encode_topology(const TopologyProjection& topology);
[[nodiscard]] CF_API std::vector<std::uint8_t> encode_capacity(
    const CapacityAvailabilityEvidence& evidence);
[[nodiscard]] CF_API std::vector<std::uint8_t> encode_health(const SourceHealthEvidence& evidence);
[[nodiscard]] CF_API std::vector<std::uint8_t> encode_authority(
    const ServiceAuthorityEvidence& evidence);
[[nodiscard]] CF_API std::vector<std::uint8_t> encode_policy(const RedundancyPolicy& policy);
[[nodiscard]] CF_API std::vector<std::uint8_t> encode_obligations(const ObligationSet& obligations);
[[nodiscard]] CF_API std::vector<std::uint8_t> encode_plan(const FailoverPlan& plan);
[[nodiscard]] CF_API std::vector<std::uint8_t> encode_plan_state(PlanId plan, PlanState state,
                                                                 FenceReason reason, Tick at);
[[nodiscard]] CF_API std::vector<std::uint8_t> encode_command(const CommandRecord& command);
[[nodiscard]] CF_API std::vector<std::uint8_t> encode_observation(
    const EffectObservationRecord& observation);
[[nodiscard]] CF_API std::vector<std::uint8_t> encode_attempt(const TransitionAttempt& attempt);
[[nodiscard]] CF_API std::vector<std::uint8_t> encode_health_streak(const HealthStreak& streak);
[[nodiscard]] CF_API std::vector<std::uint8_t> encode_transition(const TransitionRecord& record);

}  // namespace cooling_failover
