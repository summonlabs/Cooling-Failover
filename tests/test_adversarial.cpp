// Adversarial hardening: hostile evidence, hostile paths, hostile lifecycles.
#include "harness.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace cooling_failover;

namespace {

const ControlPlaneEpoch kEpoch = ControlPlaneEpoch::from_value(1);

int run_shell(const std::string& command) { return std::system(command.c_str()); }

}  // namespace

CF_TEST(Adversarial, EvidenceDatedInTheFutureIsIndeterminate) {
  cf_test::TempDir dir("adversarial-future");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(
                                                 FailoverDomainId::from_value(90))));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  // The capacity snapshot claims a later observation time than the evaluation.
  harness->facility.set_capacity_generation(CapacityGeneration::from_value(2));
  const CapacityAvailabilityEvidence future =
      harness->facility.capacity(harness->new_evidence(), Tick::from_value(500));
  CF_CHECK_OK(harness->orchestrator->record_capacity(future));

  CF_ASSIGN_OR_FAIL(set, harness->orchestrator->evaluate_candidates(Tick::from_value(10)));
  CF_CHECK(!set.candidates.empty());
  for (const FailoverCandidate& candidate : set.candidates) {
    CF_CHECK_EQ(candidate.state, EligibilityState::Indeterminate);
    CF_CHECK(std::find(candidate.causes.begin(), candidate.causes.end(),
                       EligibilityCause::EvidenceFromFuture) != candidate.causes.end());
  }
  CF_CHECK_ERROR(harness->orchestrator->create_plan(
                     PlanRequest{RequestId::from_value(1), Tick::from_value(10),
                                 PlanKind::Failover, SourceGroupId::from_value(1),
                                 SourceGroupId::nil(), "future evidence"}),
                 ErrorCode::NoEligibleCandidate);
}

CF_TEST(Adversarial, UnreachableControlRuntimeIsRecordedNotHidden) {
  cf_test::TempDir dir("adversarial-runtime");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(
                                                 FailoverDomainId::from_value(91))));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  harness->now = Tick::from_value(5);
  harness->facility.fail_all_except({SourceGroupId::from_value(2)});
  CF_CHECK_OK(harness->publish_health(CoolingSourceId::from_value(1)));
  CF_CHECK_OK(harness->publish_health(CoolingSourceId::from_value(2)));

  synthetic::SyntheticRuntimeBehaviour offline;
  offline.reachable = false;
  harness->liquid.set_behaviour(offline);
  harness->airflow.set_behaviour(offline);

  CF_ASSIGN_OR_FAIL(plan, harness->orchestrator->create_plan(
                              PlanRequest{RequestId::from_value(1), harness->now,
                                          PlanKind::Failover, SourceGroupId::from_value(1),
                                          SourceGroupId::nil(), "runtime offline"}));
  CF_ASSIGN_OR_FAIL(attempt, harness->orchestrator->execute_plan(plan.id, harness->now));
  CF_CHECK_EQ(attempt.state, AttemptState::Failed);
  CF_CHECK(attempt.verified_effects.empty());
  for (const auto& entry : harness->orchestrator->state().commands) {
    CF_CHECK_EQ(entry.second.outcome, CommandOutcome::RuntimeUnavailable);
  }
  CF_ASSIGN_OR_FAIL(held, harness->orchestrator->get_plan(plan.id));
  CF_CHECK_EQ(held.state, PlanState::Active);  // never completed
}

CF_TEST(Adversarial, EpochAdvanceFencesEveryActivePlan) {
  cf_test::TempDir dir("adversarial-epoch");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(
                                                 FailoverDomainId::from_value(92))));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  harness->now = Tick::from_value(5);
  harness->facility.fail_all_except({SourceGroupId::from_value(2)});
  CF_CHECK_OK(harness->publish_health(CoolingSourceId::from_value(1)));
  CF_CHECK_OK(harness->publish_health(CoolingSourceId::from_value(2)));
  CF_ASSIGN_OR_FAIL(plan, harness->orchestrator->create_plan(
                              PlanRequest{RequestId::from_value(1), harness->now,
                                          PlanKind::Failover, SourceGroupId::from_value(1),
                                          SourceGroupId::nil(), "before epoch advance"}));
  const SourceGroupId target = plan.target;

  // Authority for a new epoch arrives; establishing it supersedes the old epoch.
  const ControlPlaneEpoch next = ControlPlaneEpoch::from_value(2);
  harness->now = Tick::from_value(6);
  CF_CHECK_OK(harness->publish_authority(AuthorityState::Granted, next));
  CF_CHECK_OK(harness->orchestrator->establish_epoch(next, harness->now));

  CF_ASSIGN_OR_FAIL(fenced, harness->orchestrator->get_plan(plan.id));
  CF_CHECK_EQ(fenced.state, PlanState::Fenced);
  CF_CHECK_EQ(fenced.fence_reason, FenceReason::EpochSuperseded);
  CF_CHECK_EQ(fenced.target, target);
  CF_CHECK_ERROR(harness->orchestrator->execute_plan(plan.id, harness->now),
                 ErrorCode::PlanFenced);

  // An older epoch can never be re-established.
  CF_CHECK_ERROR(harness->orchestrator->establish_epoch(kEpoch, harness->now),
                 ErrorCode::SupersededEpoch);
}

CF_TEST(Adversarial, CapacityGenerationRegressionIsRefused) {
  cf_test::TempDir dir("adversarial-regression");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(
                                                 FailoverDomainId::from_value(93))));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  harness->facility.set_capacity_generation(CapacityGeneration::from_value(4));
  harness->now = Tick::from_value(1);
  CF_CHECK_OK(harness->publish_capacity());

  const CapacityAvailabilityEvidence older =
      harness->facility.capacity(harness->new_evidence(), Tick::from_value(2));
  CapacityAvailabilityEvidence regressed = older;
  regressed.generation = CapacityGeneration::from_value(3);
  CF_CHECK_ERROR(harness->orchestrator->record_capacity(regressed),
                 ErrorCode::StaleGeneration);
  CF_CHECK_EQ(harness->orchestrator->state().capacity.generation.value(), std::uint64_t{4});
}

CF_TEST(Adversarial, ClosedOrchestratorRefusesFurtherMutation) {
  cf_test::TempDir dir("adversarial-closed");
  const FailoverDomainId domain = FailoverDomainId::from_value(94);
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(domain)));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  const std::uint64_t commit = harness->orchestrator->state().last_commit.value();
  CF_CHECK_OK(harness->orchestrator->close());
  CF_CHECK(harness->orchestrator->closed());
  CF_CHECK_ERROR(harness->orchestrator->register_domain(harness->now),
                 ErrorCode::StoreClosed);
  CF_CHECK_ERROR(harness->orchestrator->evaluate_candidates(harness->now),
                 ErrorCode::StoreClosed);
  CF_CHECK_ERROR(harness->orchestrator->status(), ErrorCode::StoreClosed);
  CF_CHECK_ERROR(harness->orchestrator->checkpoint(), ErrorCode::StoreClosed);
  CF_CHECK_OK(harness->orchestrator->close());  // repeated close is safe

  // The durable generation is untouched and reopening works.
  CF_CHECK_OK(harness->restart());
  CF_CHECK_EQ(harness->orchestrator->state().last_commit.value(), commit);
}

CF_TEST(Adversarial, RepeatedOpenCloseCyclesAreSafe) {
  cf_test::TempDir dir("adversarial-cycles");
  const FailoverDomainId domain = FailoverDomainId::from_value(95);
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(domain)));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  const std::string first_digest = harness->orchestrator->state_digest().to_hex();
  for (int cycle = 0; cycle < 5; ++cycle) {
    CF_CHECK_OK(harness->orchestrator->close());
    CF_CHECK_OK(harness->restart());
    CF_CHECK_EQ(harness->orchestrator->state_digest().to_hex(), first_digest);
  }
  // A second orchestrator over the same store is refused while one is open.
  OrchestratorOptions options;
  options.store.root = dir.root();
  CF_CHECK_ERROR(CoolingFailoverOrchestrator::open(options, domain), ErrorCode::StoreLocked);
}

CF_TEST(Adversarial, ReparsePointStoreRootIsRefused) {
  cf_test::TempDir dir("adversarial-reparse");
  const std::filesystem::path target = dir.path() / "real-root";
  const std::filesystem::path link = dir.path() / "linked-root";
  std::error_code code;
  std::filesystem::create_directories(target, code);
  CF_CHECK_MSG(!code, "target directory could not be created");
  const std::string command =
      "cmd /c mklink /J \"" + link.string() + "\" \"" + target.string() + "\" >nul 2>&1";
  const int created = run_shell(command);
  CF_CHECK_MSG(created == 0, "a directory junction could not be created for this test");
  CF_CHECK(std::filesystem::exists(link / ".", code));
  CF_CHECK_ERROR(prepare_store_root(link), ErrorCode::PathReparsePoint);
  // The real directory is still usable.
  CF_ASSIGN_OR_FAIL(prepared, prepare_store_root(target));
  CF_CHECK(std::filesystem::is_directory(prepared));
}

CF_TEST(Adversarial, PolicyAbsenceRefusesPlanning) {
  cf_test::TempDir dir("adversarial-policy");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(
                                                 FailoverDomainId::from_value(96))));
  CF_CHECK_OK(harness->orchestrator->register_domain(harness->now));
  CF_CHECK_OK(harness->publish_topology());
  CF_CHECK_OK(harness->publish_capacity());
  CF_CHECK_OK(harness->publish_obligations());
  CF_CHECK_OK(harness->publish_authority(AuthorityState::Granted, kEpoch));
  CF_CHECK_OK(harness->publish_all_health());
  CF_CHECK_OK(harness->orchestrator->establish_epoch(kEpoch, harness->now));
  CF_CHECK_ERROR(harness->orchestrator->create_plan(
                     PlanRequest{RequestId::from_value(1), harness->now, PlanKind::Failover,
                                 SourceGroupId::from_value(1), SourceGroupId::nil(), "no policy"}),
                 ErrorCode::PolicyNotDefined);
  CF_CHECK_ERROR(harness->orchestrator->evaluate_candidates(harness->now),
                 ErrorCode::PolicyNotDefined);
}

CF_TEST(Adversarial, ManualOnlyPolicyRefusesAutomaticSelection) {
  cf_test::TempDir dir("adversarial-manual");
  synthetic::SyntheticSpec spec = synthetic::two_group_plant(FailoverDomainId::from_value(97));
  spec.policy.selection = SelectionMode::ManualOnly;
  spec.policy.rank_order.clear();
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(dir.root(), spec));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  harness->now = Tick::from_value(5);
  harness->facility.fail_all_except({SourceGroupId::from_value(2)});
  CF_CHECK_OK(harness->publish_health(CoolingSourceId::from_value(1)));
  CF_CHECK_OK(harness->publish_health(CoolingSourceId::from_value(2)));

  CF_CHECK_ERROR(harness->orchestrator->create_plan(
                     PlanRequest{RequestId::from_value(1), harness->now, PlanKind::Failover,
                                 SourceGroupId::from_value(1), SourceGroupId::nil(),
                                 "no ordering defined"}),
                 ErrorCode::SelectionNotDefined);

  CF_ASSIGN_OR_FAIL(set, harness->orchestrator->evaluate_candidates(harness->now));
  CF_CHECK(set.selected.is_nil());

  // An explicit target is still honoured because the caller named it.
  CF_ASSIGN_OR_FAIL(plan, harness->orchestrator->create_plan(
                              PlanRequest{RequestId::from_value(2), harness->now,
                                          PlanKind::Failover, SourceGroupId::from_value(1),
                                          SourceGroupId::from_value(2), "named target"}));
  CF_CHECK_EQ(plan.target_groups.front(), SourceGroupId::from_value(2));
}

CF_TEST(Adversarial, FailoverCannotTargetItsOwnIncumbent) {
  cf_test::TempDir dir("adversarial-self");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(
                                                 FailoverDomainId::from_value(98))));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  CF_CHECK_ERROR(harness->orchestrator->create_plan(
                     PlanRequest{RequestId::from_value(1), harness->now, PlanKind::Failover,
                                 SourceGroupId::from_value(1), SourceGroupId::from_value(1),
                                 "self failover"}),
                 ErrorCode::InvalidArgument);
}

CF_TEST(Adversarial, UnknownPlanAndAttemptAreRefused) {
  cf_test::TempDir dir("adversarial-unknown");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(
                                                 FailoverDomainId::from_value(99))));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  CF_CHECK_ERROR(harness->orchestrator->get_plan(PlanId::from_value(0xDEAD)),
                 ErrorCode::PlanNotFound);
  CF_CHECK_ERROR(harness->orchestrator->get_attempt(AttemptId::from_value(0xDEAD)),
                 ErrorCode::AttemptNotFound);
  CF_CHECK_ERROR(harness->orchestrator->execute_plan(PlanId::from_value(0xDEAD), harness->now),
                 ErrorCode::PlanNotFound);
  CF_CHECK_ERROR(harness->orchestrator->fence_plan(PlanId::from_value(0xDEAD),
                                                   FenceReason::Manual, harness->now),
                 ErrorCode::PlanNotFound);
  CF_CHECK_ERROR(harness->orchestrator->verify_attempt(AttemptId::from_value(0xDEAD)),
                 ErrorCode::AttemptNotFound);
}

CF_TEST(Adversarial, NilIdentitiesAreRefused) {
  cf_test::TempDir dir("adversarial-nil");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(
                                                 FailoverDomainId::from_value(100))));
  CF_CHECK_OK(harness->orchestrator->register_domain(harness->now));
  CF_CHECK_OK(harness->publish_topology());
  CF_CHECK_ERROR(harness->orchestrator->record_capacity(
                     harness->facility.capacity(EvidenceId::nil(), harness->now)),
                 ErrorCode::InvalidArgument);
  CF_CHECK_ERROR(harness->orchestrator->record_health(
                     harness->facility.health_for(CoolingSourceId::nil(),
                                                  harness->new_evidence(), harness->now)),
                 ErrorCode::InvalidArgument);
  CF_CHECK_ERROR(harness->orchestrator->create_plan(
                     PlanRequest{RequestId::nil(), harness->now, PlanKind::Failover,
                                 SourceGroupId::from_value(1), SourceGroupId::nil(), "nil request"}),
                 ErrorCode::InvalidArgument);
  CF_CHECK_ERROR(
      CoolingFailoverOrchestrator::open(OrchestratorOptions{}, FailoverDomainId::nil()),
      ErrorCode::InvalidArgument);
}

CF_TEST(Adversarial, DuplicateDomainRegistrationIsRefused) {
  cf_test::TempDir dir("adversarial-duplicate");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(
                                                 FailoverDomainId::from_value(101))));
  CF_CHECK_OK(harness->orchestrator->register_domain(harness->now));
  CF_CHECK_ERROR(harness->orchestrator->register_domain(harness->now),
                 ErrorCode::DuplicateIdentity);
}

CF_TEST(Adversarial, HealthEvidenceForUnknownSourceIsRefused) {
  cf_test::TempDir dir("adversarial-health");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(
                                                 FailoverDomainId::from_value(102))));
  CF_CHECK_OK(harness->orchestrator->register_domain(harness->now));
  CF_CHECK_ERROR(harness->orchestrator->record_health(harness->facility.health_for(
                     CoolingSourceId::from_value(77), harness->new_evidence(), harness->now)),
                 ErrorCode::TopologyNotImported);
  CF_CHECK_OK(harness->publish_topology());
  CF_CHECK_ERROR(harness->orchestrator->record_health(harness->facility.health_for(
                     CoolingSourceId::from_value(77), harness->new_evidence(), harness->now)),
                 ErrorCode::InvalidArgument);
}

CF_TEST(Adversarial, UnopenableStoreFileIsReportedAsIoNotMissing) {
  cf_test::TempDir dir("adversarial-permission");
  const FailoverDomainId domain = FailoverDomainId::from_value(103);
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(domain)));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  CF_CHECK_OK(harness->orchestrator->close());

  // Locate the write-ahead log through the same directory naming the store uses.
  std::string name;
  static constexpr char kDigits[] = "0123456789abcdef";
  for (int shift = 60; shift >= 0; shift -= 4) {
    name.push_back(kDigits[(domain.value() >> shift) & 0xFU]);
  }
  const std::filesystem::path wal =
      std::filesystem::weakly_canonical(dir.root()) / name / "wal.log";
  CF_CHECK(std::filesystem::exists(wal));

  const auto attributes = std::filesystem::status(wal).permissions();
  std::error_code code;
  std::filesystem::permissions(wal, std::filesystem::perms::owner_read,
                               std::filesystem::perm_options::replace, code);
  CF_CHECK_MSG(!code, "permissions could not be changed for this test");

  const cooling_failover::Result<std::unique_ptr<cf_test::DomainHarness>> outcome =
      cf_test::DomainHarness::open(dir.root(), synthetic::two_group_plant(domain));
  std::filesystem::permissions(wal, attributes, std::filesystem::perm_options::replace, code);
  CF_CHECK_MSG(!outcome.has_value(), "an unopenable log must not be treated as a usable store");
  CF_CHECK_EQ(outcome.status().code(), ErrorCode::StoreIoError);

  CF_ASSIGN_OR_FAIL(reopened, cf_test::DomainHarness::open(
                                  dir.root(), synthetic::two_group_plant(domain)));
  CF_CHECK_OK(reopened->orchestrator->close());
}
