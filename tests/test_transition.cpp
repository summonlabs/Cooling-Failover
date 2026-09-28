// Transition semantics: acknowledgement, partial effects, fencing and replay.
#include "harness.hpp"

#include <algorithm>
#include <set>
#include <sstream>

using namespace cooling_failover;
using cooling_failover::synthetic::SyntheticRuntimeBehaviour;

namespace {

const ControlPlaneEpoch kEpoch = ControlPlaneEpoch::from_value(1);
const SourceGroupId kPrimary = SourceGroupId::from_value(1);
const SourceGroupId kAlternate = SourceGroupId::from_value(2);

/// Makes the primary group unavailable and republishes health for both sources.
cooling_failover::Result<void> degrade_primary(cf_test::DomainHarness& domain, Tick now) {
  domain.now = now;
  domain.facility.fail_all_except({kAlternate});
  CF_TRY(domain.publish_health(CoolingSourceId::from_value(1)));
  CF_TRY(domain.publish_health(CoolingSourceId::from_value(2)));
  return cooling_failover::Result<void>();
}

PlanRequest failover_request(std::uint64_t id, Tick now) {
  PlanRequest request;
  request.request = RequestId::from_value(id);
  request.now = now;
  request.kind = PlanKind::Failover;
  request.incumbent = kPrimary;
  request.reason = "primary unavailable";
  return request;
}

}  // namespace

CF_TEST(Transition, VerifiedFailoverRequiresObservedEffects) {
  cf_test::TempDir dir("transition-verified");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(
                                                 FailoverDomainId::from_value(20))));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  CF_CHECK_OK(degrade_primary(*harness, Tick::from_value(5)));

  CF_ASSIGN_OR_FAIL(plan, harness->orchestrator->create_plan(failover_request(1, harness->now)));
  CF_CHECK(plan.target_groups.size() == 1);
  CF_CHECK_EQ(plan.target_groups.front(), kAlternate);
  CF_CHECK(plan.state == PlanState::Active);

  CF_ASSIGN_OR_FAIL(attempt, harness->orchestrator->execute_plan(plan.id, harness->now));
  CF_CHECK_EQ(attempt.state, AttemptState::Verified);
  CF_ASSIGN_OR_FAIL(summary, harness->orchestrator->summarize_attempt(attempt.id));
  CF_CHECK(summary.complete);
  CF_CHECK(summary.missing.empty());
  CF_CHECK(harness->liquid.actuations() > 0);

  CF_ASSIGN_OR_FAIL(completed, harness->orchestrator->get_plan(plan.id));
  CF_CHECK_EQ(completed.state, PlanState::Completed);
  CF_ASSIGN_OR_FAIL(status, harness->orchestrator->status());
  CF_CHECK(status.active_plan.is_nil());
  CF_CHECK(!status.degraded);
}

CF_TEST(Transition, AcknowledgementIsNotProofOfTransfer) {
  cf_test::TempDir dir("transition-ack");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(
                                                 FailoverDomainId::from_value(21))));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  CF_CHECK_OK(degrade_primary(*harness, Tick::from_value(5)));

  SyntheticRuntimeBehaviour quiet;
  quiet.emits_effects = false;
  harness->liquid.set_behaviour(quiet);

  CF_ASSIGN_OR_FAIL(plan, harness->orchestrator->create_plan(failover_request(2, harness->now)));
  CF_ASSIGN_OR_FAIL(attempt, harness->orchestrator->execute_plan(plan.id, harness->now));
  CF_CHECK(attempt.state != AttemptState::Verified);
  CF_CHECK_EQ(attempt.state, AttemptState::Acknowledged);
  CF_CHECK(harness->liquid.actuations() > 0);

  CF_ASSIGN_OR_FAIL(held, harness->orchestrator->get_plan(plan.id));
  CF_CHECK_EQ(held.state, PlanState::Active);
  CF_ASSIGN_OR_FAIL(status, harness->orchestrator->status());
  CF_CHECK(status.degraded || status.active_attempt_state == AttemptState::Acknowledged);
}

CF_TEST(Transition, AcknowledgementOriginNeverVerifies) {
  cf_test::TempDir dir("transition-ack-origin");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(
                                                 FailoverDomainId::from_value(22))));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  CF_CHECK_OK(degrade_primary(*harness, Tick::from_value(5)));

  SyntheticRuntimeBehaviour ack_only;
  ack_only.acknowledgement_only = true;
  harness->liquid.set_behaviour(ack_only);

  CF_ASSIGN_OR_FAIL(plan, harness->orchestrator->create_plan(failover_request(3, harness->now)));
  CF_ASSIGN_OR_FAIL(attempt, harness->orchestrator->execute_plan(plan.id, harness->now));
  CF_CHECK(attempt.state != AttemptState::Verified);
  CF_CHECK(!harness->orchestrator->state().observations.empty());
  for (const auto& entry : harness->orchestrator->state().observations) {
    CF_CHECK_EQ(entry.second.origin, ObservationOrigin::ActuationControllerAck);
  }
}

CF_TEST(Transition, PartialTransitionIsNeverCompletion) {
  cf_test::TempDir dir("transition-partial");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(
                                                 FailoverDomainId::from_value(23))));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  CF_CHECK_OK(degrade_primary(*harness, Tick::from_value(5)));

  SyntheticRuntimeBehaviour partial;
  partial.suppressed_effects = {EffectKind::ReturnTemperatureInBand};
  harness->liquid.set_behaviour(partial);

  CF_ASSIGN_OR_FAIL(plan, harness->orchestrator->create_plan(failover_request(4, harness->now)));
  CF_ASSIGN_OR_FAIL(attempt, harness->orchestrator->execute_plan(plan.id, harness->now));
  CF_CHECK_EQ(attempt.state, AttemptState::PartiallyObserved);
  CF_CHECK(!attempt.verified_effects.empty());
  CF_CHECK(!attempt.missing_effects.empty());
  CF_CHECK(std::find(attempt.missing_effects.begin(), attempt.missing_effects.end(),
                     EffectKind::ReturnTemperatureInBand) != attempt.missing_effects.end());

  CF_ASSIGN_OR_FAIL(held, harness->orchestrator->get_plan(plan.id));
  CF_CHECK_EQ(held.state, PlanState::Active);  // not complete
  CF_ASSIGN_OR_FAIL(status, harness->orchestrator->status());
  CF_CHECK(status.degraded);
}

CF_TEST(Transition, StaleEvidenceNeverVerifies) {
  cf_test::TempDir dir("transition-stale");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(
                                                 FailoverDomainId::from_value(24))));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  CF_CHECK_OK(degrade_primary(*harness, Tick::from_value(5)));

  // The controller reports effects stamped with generations the orchestrator no
  // longer holds, so nothing may verify.
  harness->liquid.set_generations(TopologyGeneration::from_value(99),
                                  CapacityGeneration::from_value(99));

  CF_ASSIGN_OR_FAIL(plan, harness->orchestrator->create_plan(failover_request(5, harness->now)));
  CF_ASSIGN_OR_FAIL(attempt, harness->orchestrator->execute_plan(plan.id, harness->now));
  CF_CHECK(attempt.state != AttemptState::Verified);
  CF_CHECK(attempt.verified_effects.empty());

  // Current evidence from the owning runtime completes the same attempt.
  harness->liquid.set_generations(harness->facility.spec().topology_generation,
                                  harness->facility.spec().capacity_generation);
  EffectSequence sequence = EffectSequence::from_value(5000000);
  for (const CommandSpec& command : plan.commands) {
    for (EffectKind effect : command.required_effects) {
      EffectObservationRecord observation;
      observation.id = ObservationId::from_value(900000 + sequence.value());
      observation.command = command.id;
      observation.group = command.target;
      observation.effect = effect;
      observation.origin = command.runtime == OwnerSystem::AirflowControl
                               ? ObservationOrigin::AirflowControl
                               : ObservationOrigin::LiquidCoolingControl;
      observation.sequence = sequence;
      observation.topology = plan.topology;
      observation.capacity = plan.capacity;
      observation.epoch = plan.epoch;
      observation.observed_at = Tick::from_value(6);
      CF_CHECK_OK(harness->orchestrator->submit_observation(observation));
      CF_ASSIGN_OR_FAIL(next, sequence.next());
      sequence = next;
    }
  }
  CF_ASSIGN_OR_FAIL(verified, harness->orchestrator->verify_attempt(attempt.id));
  CF_CHECK_EQ(verified.state, AttemptState::Verified);
  CF_ASSIGN_OR_FAIL(completed, harness->orchestrator->get_plan(plan.id));
  CF_CHECK_EQ(completed.state, PlanState::Completed);
}

CF_TEST(Transition, RepeatedExecutionDoesNotDuplicateIssuedRequests) {
  cf_test::TempDir dir("transition-replay");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(
                                                 FailoverDomainId::from_value(25))));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  CF_CHECK_OK(degrade_primary(*harness, Tick::from_value(5)));

  SyntheticRuntimeBehaviour quiet;
  quiet.emits_effects = false;
  harness->liquid.set_behaviour(quiet);
  harness->airflow.set_behaviour(quiet);

  CF_ASSIGN_OR_FAIL(plan, harness->orchestrator->create_plan(failover_request(6, harness->now)));
  CF_ASSIGN_OR_FAIL(first, harness->orchestrator->execute_plan(plan.id, harness->now));
  const std::uint32_t first_actuations = harness->liquid.actuations();
  const std::uint32_t first_issue_calls = harness->liquid.issue_calls();
  const std::size_t command_records = harness->orchestrator->state().commands.size();
  CF_CHECK(first_actuations > 0);

  CF_ASSIGN_OR_FAIL(second, harness->orchestrator->execute_plan(plan.id, harness->now));
  CF_CHECK_EQ(harness->liquid.actuations(), first_actuations);
  CF_CHECK_EQ(harness->liquid.issue_calls(), first_issue_calls);
  CF_CHECK_EQ(harness->orchestrator->state().commands.size(), command_records);
  CF_CHECK_EQ(first.id, second.id);
}

CF_TEST(Transition, PlanRequestIdentityIsIdempotentAndConflictingReuseIsRefused) {
  cf_test::TempDir dir("transition-request");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(
                                                 FailoverDomainId::from_value(26))));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  CF_CHECK_OK(degrade_primary(*harness, Tick::from_value(5)));

  const PlanRequest request = failover_request(7, harness->now);
  CF_ASSIGN_OR_FAIL(original, harness->orchestrator->create_plan(request));
  CF_ASSIGN_OR_FAIL(replayed, harness->orchestrator->create_plan(request));
  CF_CHECK_EQ(original.id, replayed.id);

  PlanRequest conflicting = request;
  conflicting.reason = "different reason";
  CF_CHECK_ERROR(harness->orchestrator->create_plan(conflicting), ErrorCode::IdempotencyConflict);
}

CF_TEST(Transition, ConcurrentPlansForOneDomainAreExcluded) {
  cf_test::TempDir dir("transition-concurrent");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(
                                                 FailoverDomainId::from_value(27))));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  CF_CHECK_OK(degrade_primary(*harness, Tick::from_value(5)));
  CF_ASSIGN_OR_FAIL(original, harness->orchestrator->create_plan(failover_request(8, harness->now)));
  CF_CHECK_ERROR(harness->orchestrator->create_plan(failover_request(9, harness->now)),
                 ErrorCode::PlanAlreadyActive);
  CF_ASSIGN_OR_FAIL(still, harness->orchestrator->get_plan(original.id));
  CF_CHECK_EQ(still.state, PlanState::Active);
}

CF_TEST(Transition, LossOfAuthorityFencesAnInFlightPlan) {
  cf_test::TempDir dir("transition-authority");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(
                                                 FailoverDomainId::from_value(28))));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  CF_CHECK_OK(degrade_primary(*harness, Tick::from_value(5)));

  SyntheticRuntimeBehaviour quiet;
  quiet.emits_effects = false;
  harness->liquid.set_behaviour(quiet);

  CF_ASSIGN_OR_FAIL(plan, harness->orchestrator->create_plan(failover_request(10, harness->now)));
  CF_ASSIGN_OR_FAIL(attempt, harness->orchestrator->execute_plan(plan.id, harness->now));
  CF_CHECK(attempt.state != AttemptState::Verified);

  harness->now = Tick::from_value(7);
  CF_CHECK_OK(harness->publish_authority(AuthorityState::Denied, kEpoch));

  CF_ASSIGN_OR_FAIL(fenced, harness->orchestrator->get_plan(plan.id));
  CF_CHECK_EQ(fenced.state, PlanState::Fenced);
  CF_CHECK_EQ(fenced.fence_reason, FenceReason::AuthorityLost);
  CF_ASSIGN_OR_FAIL(fenced_attempt, harness->orchestrator->get_attempt(attempt.id));
  CF_CHECK_EQ(fenced_attempt.state, AttemptState::Fenced);

  // No further command may be issued under fenced authority.
  const std::uint32_t actuations = harness->liquid.actuations();
  CF_CHECK_ERROR(harness->orchestrator->execute_plan(plan.id, harness->now), ErrorCode::PlanFenced);
  CF_CHECK_EQ(harness->liquid.actuations(), actuations);
}

CF_TEST(Transition, GenerationChangeFencesAPlan) {
  cf_test::TempDir dir("transition-generation");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(
                                                 FailoverDomainId::from_value(29))));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  CF_CHECK_OK(degrade_primary(*harness, Tick::from_value(5)));
  CF_ASSIGN_OR_FAIL(plan, harness->orchestrator->create_plan(failover_request(11, harness->now)));

  harness->facility.set_capacity_generation(CapacityGeneration::from_value(2));
  harness->now = Tick::from_value(6);
  CF_CHECK_OK(harness->publish_capacity());

  CF_ASSIGN_OR_FAIL(fenced, harness->orchestrator->get_plan(plan.id));
  CF_CHECK_EQ(fenced.state, PlanState::Fenced);
  CF_CHECK_EQ(fenced.fence_reason, FenceReason::CapacityGenerationChanged);
  CF_CHECK_ERROR(harness->orchestrator->execute_plan(plan.id, harness->now), ErrorCode::PlanFenced);
}

CF_TEST(Transition, CommandsAreRoutedToTheOwningRuntime) {
  cf_test::TempDir dir("transition-routing");
  synthetic::SyntheticSpec spec = synthetic::two_group_plant(FailoverDomainId::from_value(30));
  spec.groups[1].kind = SourceKind::Crah;  // air side
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(dir.root(), spec));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  CF_CHECK_OK(degrade_primary(*harness, Tick::from_value(5)));

  CF_ASSIGN_OR_FAIL(plan, harness->orchestrator->create_plan(failover_request(12, harness->now)));
  std::set<int> runtimes;
  for (const CommandSpec& command : plan.commands) {
    runtimes.insert(static_cast<int>(command.runtime));
  }
  CF_CHECK(runtimes.count(static_cast<int>(OwnerSystem::AirflowControl)) == 1);
  CF_CHECK(runtimes.count(static_cast<int>(OwnerSystem::LiquidCoolingControl)) == 1);

  CF_ASSIGN_OR_FAIL(attempt, harness->orchestrator->execute_plan(plan.id, harness->now));
  if (attempt.state != AttemptState::Verified) {
    std::ostringstream detail;
    detail << "attempt state " << to_string(attempt.state) << " verified=";
    for (EffectKind effect : attempt.verified_effects) {
      detail << to_string(effect) << ",";
    }
    detail << " missing=";
    for (EffectKind effect : attempt.missing_effects) {
      detail << to_string(effect) << ",";
    }
    detail << " commands=" << plan.commands.size();
    CF_FAIL(detail.str());
  }
  CF_CHECK(harness->airflow.actuations() > 0);
  CF_CHECK(harness->liquid.actuations() > 0);
}

CF_TEST(Transition, DuplicateObservationIdentityIsRejected) {
  cf_test::TempDir dir("transition-duplicate");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(
                                                 FailoverDomainId::from_value(31))));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  CF_CHECK_OK(degrade_primary(*harness, Tick::from_value(5)));
  SyntheticRuntimeBehaviour quiet;
  quiet.emits_effects = false;
  harness->liquid.set_behaviour(quiet);
  CF_ASSIGN_OR_FAIL(plan, harness->orchestrator->create_plan(failover_request(13, harness->now)));
  CF_ASSIGN_OR_FAIL(attempt, harness->orchestrator->execute_plan(plan.id, harness->now));
  (void)attempt;

  const CommandSpec& first = plan.commands.front();
  EffectObservationRecord observation;
  observation.id = ObservationId::from_value(4242);
  observation.command = first.id;
  observation.group = first.target;
  observation.effect = first.required_effects.front();
  observation.origin = ObservationOrigin::LiquidCoolingControl;
  observation.sequence = EffectSequence::from_value(7000000);
  observation.topology = plan.topology;
  observation.capacity = plan.capacity;
  observation.epoch = plan.epoch;
  observation.observed_at = Tick::from_value(6);
  CF_CHECK_OK(harness->orchestrator->submit_observation(observation));

  // Exact replay is accepted.
  CF_CHECK_OK(harness->orchestrator->submit_observation(observation));

  // Reuse of the identity with different content is refused.
  EffectObservationRecord conflicting = observation;
  conflicting.effect = EffectKind::CapacityDelivered;
  CF_CHECK_ERROR(harness->orchestrator->submit_observation(conflicting),
                 ErrorCode::DuplicateIdentity);

  // An observation for a command that was never issued is refused.
  EffectObservationRecord unknown = observation;
  unknown.id = ObservationId::from_value(4243);
  unknown.command = CommandId::from_value(0xDEAD);
  unknown.sequence = EffectSequence::from_value(7000001);
  CF_CHECK_ERROR(harness->orchestrator->submit_observation(unknown),
                 ErrorCode::ObservationUnknownCommand);

  // A non-monotonic sequence for the same effect stream is refused.
  EffectObservationRecord reordered = observation;
  reordered.id = ObservationId::from_value(4244);
  reordered.sequence = EffectSequence::from_value(10);
  CF_CHECK_ERROR(harness->orchestrator->submit_observation(reordered),
                 ErrorCode::EvidenceReordered);
}
