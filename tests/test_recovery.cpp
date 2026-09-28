// Recovery, hysteresis and oscillation control.
#include "harness.hpp"

#include <algorithm>

using namespace cooling_failover;

namespace {

const ControlPlaneEpoch kEpoch = ControlPlaneEpoch::from_value(1);
const SourceGroupId kPrimary = SourceGroupId::from_value(1);
const SourceGroupId kAlternate = SourceGroupId::from_value(2);

/// Fails the primary over to the alternate and completes it.
cooling_failover::Result<PlanId> fail_over(cf_test::DomainHarness& domain,
                                           std::uint64_t request_id, Tick now) {
  domain.now = now;
  domain.facility.fail_all_except({kAlternate});
  CF_TRY(domain.publish_health(CoolingSourceId::from_value(1)));
  CF_TRY(domain.publish_health(CoolingSourceId::from_value(2)));
  PlanRequest request;
  request.request = RequestId::from_value(request_id);
  request.now = now;
  request.kind = PlanKind::Failover;
  request.incumbent = kPrimary;
  request.reason = "primary unavailable";
  CF_TRY_ASSIGN(plan, domain.orchestrator->create_plan(request));
  CF_TRY_ASSIGN(attempt, domain.orchestrator->execute_plan(plan.id, now));
  if (attempt.state != AttemptState::Verified) {
    return cooling_failover::Status::error(cooling_failover::ErrorCode::PartialTransition,
                                           "synthetic failover did not verify");
  }
  return plan.id;
}

/// Publishes \p count consecutive healthy observations of the primary source.
cooling_failover::Result<void> heal_primary(cf_test::DomainHarness& domain, std::uint64_t count,
                                            Tick start, std::uint64_t spacing) {
  domain.facility.set_health(CoolingSourceId::from_value(1), HealthState::Healthy);
  for (std::uint64_t index = 0; index < count; ++index) {
    domain.now = Tick::from_value(start.value() + index * spacing);
    CF_TRY(domain.publish_health(CoolingSourceId::from_value(1)));
  }
  return cooling_failover::Result<void>();
}

}  // namespace

CF_TEST(Recovery, RequiresConsecutiveHealthyEvidenceAndDwell) {
  cf_test::TempDir dir("recovery-hysteresis");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(
                                                 FailoverDomainId::from_value(60))));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  CF_ASSIGN_OR_FAIL(plan, fail_over(*harness, 1, Tick::from_value(10)));
  (void)plan;

  CF_ASSIGN_OR_FAIL(none, harness->orchestrator->evaluate_recovery(Tick::from_value(20)));
  CF_CHECK(!none.eligible);

  // One healthy observation is not a streak.
  CF_CHECK_OK(heal_primary(*harness, 1, Tick::from_value(21), 1));
  CF_ASSIGN_OR_FAIL(short_streak, harness->orchestrator->evaluate_recovery(Tick::from_value(60)));
  CF_CHECK(!short_streak.eligible);
  CF_CHECK_EQ(short_streak.primary_cause, EligibilityCause::HealthMissing);

  // Enough consecutive observations but not enough dwell.
  CF_CHECK_OK(heal_primary(*harness, 4, Tick::from_value(22), 1));
  CF_ASSIGN_OR_FAIL(no_dwell, harness->orchestrator->evaluate_recovery(Tick::from_value(30)));
  CF_CHECK(!no_dwell.eligible);

  // Both conditions satisfied.
  CF_CHECK_OK(heal_primary(*harness, 2, Tick::from_value(100), 1));
  CF_ASSIGN_OR_FAIL(ready, harness->orchestrator->evaluate_recovery(Tick::from_value(140)));
  CF_CHECK(ready.eligible);
  CF_CHECK_EQ(ready.target, kPrimary);
  // One observation at bootstrap, one at failover (non-healthy, resetting the
  // streak) and seven healthy observations afterwards.
  CF_CHECK_EQ(ready.consecutive_healthy, 7U);
  CF_CHECK(ready.dwell_ticks >= 30);
}

CF_TEST(Recovery, NonHealthyEvidenceResetsTheStreak) {
  cf_test::TempDir dir("recovery-reset");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(
                                                 FailoverDomainId::from_value(61))));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  CF_ASSIGN_OR_FAIL(plan, fail_over(*harness, 1, Tick::from_value(10)));
  (void)plan;
  CF_CHECK_OK(heal_primary(*harness, 3, Tick::from_value(20), 1));
  harness->facility.set_health(CoolingSourceId::from_value(1), HealthState::Degraded);
  harness->now = Tick::from_value(30);
  CF_CHECK_OK(harness->publish_health(CoolingSourceId::from_value(1)));
  CF_ASSIGN_OR_FAIL(decision, harness->orchestrator->evaluate_recovery(Tick::from_value(200)));
  CF_CHECK(!decision.eligible);
  CF_CHECK_EQ(decision.consecutive_healthy, 0U);
}

CF_TEST(Recovery, OscillationGuardRefusesRapidTransitions) {
  cf_test::TempDir dir("recovery-oscillation");
  synthetic::SyntheticSpec spec = synthetic::two_group_plant(FailoverDomainId::from_value(62));
  spec.policy.oscillation_window_ticks = 1000;
  spec.policy.max_transitions_per_window = 1;
  spec.policy.recovery_consecutive_healthy = 1;
  spec.policy.recovery_dwell_ticks = 1;
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(dir.root(), spec));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  CF_ASSIGN_OR_FAIL(plan, fail_over(*harness, 1, Tick::from_value(10)));
  (void)plan;

  CF_CHECK_OK(heal_primary(*harness, 1, Tick::from_value(20), 1));
  CF_ASSIGN_OR_FAIL(blocked, harness->orchestrator->evaluate_recovery(Tick::from_value(30)));
  CF_CHECK(!blocked.eligible);
  CF_CHECK_EQ(blocked.transitions_in_window, 1U);
  CF_CHECK(blocked.detail.find("oscillation") != std::string::npos);

  // The guard releases once the window has passed.
  CF_CHECK_OK(heal_primary(*harness, 1, Tick::from_value(2000), 1));
  CF_ASSIGN_OR_FAIL(allowed, harness->orchestrator->evaluate_recovery(Tick::from_value(2100)));
  CF_CHECK(allowed.eligible);
}

CF_TEST(Recovery, RequiresCurrentEligibilityOfThePrimaryArrangement) {
  cf_test::TempDir dir("recovery-evidence");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(
                                                 FailoverDomainId::from_value(63))));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  CF_ASSIGN_OR_FAIL(plan, fail_over(*harness, 1, Tick::from_value(10)));
  (void)plan;
  CF_CHECK_OK(heal_primary(*harness, 4, Tick::from_value(20), 1));

  // The primary is health-reported but its reserve block is unavailable.
  harness->facility.set_block(ReserveBlockId::from_value(1), AvailabilityState::Unavailable,
                              Watts::zero());
  harness->now = Tick::from_value(30);
  CF_CHECK_OK(harness->publish_capacity());
  CF_ASSIGN_OR_FAIL(decision, harness->orchestrator->evaluate_recovery(Tick::from_value(90)));
  CF_CHECK(!decision.eligible);
  // Causes are reported in evaluation order; the reserve block going out of
  // service is the first decisive cause and therefore the primary one.
  CF_CHECK_EQ(decision.primary_cause, EligibilityCause::BlockUnavailable);

  // Restoring the block makes the same request eligible without new hysteresis.
  harness->facility.set_block(ReserveBlockId::from_value(1), AvailabilityState::Available,
                              Watts::from_watts(400000));
  harness->now = Tick::from_value(31);
  CF_CHECK_OK(harness->publish_capacity());
  CF_ASSIGN_OR_FAIL(restored, harness->orchestrator->evaluate_recovery(Tick::from_value(90)));
  CF_CHECK(restored.eligible);
}

CF_TEST(Recovery, ReturnToPrimaryPlanCompletes) {
  cf_test::TempDir dir("recovery-return");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(
                                                 FailoverDomainId::from_value(64))));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  CF_ASSIGN_OR_FAIL(plan, fail_over(*harness, 1, Tick::from_value(10)));
  (void)plan;
  CF_CHECK_OK(heal_primary(*harness, 6, Tick::from_value(40), 1));
  CF_ASSIGN_OR_FAIL(decision, harness->orchestrator->evaluate_recovery(Tick::from_value(120)));
  CF_CHECK(decision.eligible);

  PlanRequest request;
  request.request = RequestId::from_value(2);
  request.now = Tick::from_value(120);
  request.kind = PlanKind::ReturnToPrimary;
  request.incumbent = kAlternate;
  request.reason = "primary healthy again";
  CF_ASSIGN_OR_FAIL(return_plan, harness->orchestrator->create_plan(request));
  CF_CHECK_EQ(return_plan.target_groups.front(), kPrimary);
  CF_ASSIGN_OR_FAIL(attempt, harness->orchestrator->execute_plan(return_plan.id, Tick::from_value(120)));
  CF_CHECK_EQ(attempt.state, AttemptState::Verified);
  CF_ASSIGN_OR_FAIL(completed, harness->orchestrator->get_plan(return_plan.id));
  CF_CHECK_EQ(completed.state, PlanState::Completed);
}
