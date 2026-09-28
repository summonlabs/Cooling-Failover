// Eligibility, reserve accounting and candidate determinism.
#include "harness.hpp"

#include <algorithm>
#include <set>

using namespace cooling_failover;

namespace {

const ControlPlaneEpoch kEpoch = ControlPlaneEpoch::from_value(1);

}  // namespace

CF_TEST(Eligibility, StructurallyRedundantIsNotAutomaticallyEligible) {
  cf_test::TempDir dir("eligibility-redundant");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(),
                                 synthetic::two_group_plant(FailoverDomainId::from_value(7))));
  CF_CHECK_OK(harness->bootstrap(kEpoch));

  CandidateKey alternate;
  alternate.groups.push_back(SourceGroupId::from_value(2));
  {
    CF_ASSIGN_OR_FAIL(before, harness->orchestrator->evaluate_candidates(harness->now));
    const FailoverCandidate* healthy = before.find(alternate);
    CF_CHECK(healthy != nullptr);
    CF_CHECK(healthy->is_eligible());
  }

  // Structure is unchanged; only the evidence changes.
  harness->facility.set_health(CoolingSourceId::from_value(2), HealthState::Unavailable);
  harness->now = Tick::from_value(5);
  CF_CHECK_OK(harness->publish_health(CoolingSourceId::from_value(2)));

  CF_ASSIGN_OR_FAIL(after, harness->orchestrator->evaluate_candidates(harness->now));
  const FailoverCandidate* degraded = after.find(alternate);
  CF_CHECK(degraded != nullptr);
  CF_CHECK(!degraded->is_eligible());
  CF_CHECK_EQ(degraded->state, EligibilityState::Ineligible);
  CF_CHECK(std::find(degraded->causes.begin(), degraded->causes.end(),
                     EligibilityCause::HealthUnavailable) != degraded->causes.end());
}

CF_TEST(Eligibility, MissingEvidenceIsIndeterminateNotEligible) {
  cf_test::TempDir dir("eligibility-missing");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(),
                                 synthetic::two_group_plant(FailoverDomainId::from_value(8))));
  CF_CHECK_OK(harness->orchestrator->register_domain(harness->now));
  CF_CHECK_OK(harness->publish_topology());
  CF_CHECK_OK(harness->publish_policy());
  CF_CHECK_OK(harness->publish_obligations());
  CF_CHECK_OK(harness->publish_authority(AuthorityState::Granted, kEpoch));
  CF_CHECK_OK(harness->orchestrator->establish_epoch(kEpoch, harness->now));

  CF_ASSIGN_OR_FAIL(set, harness->orchestrator->evaluate_candidates(harness->now));
  CF_CHECK(!set.candidates.empty());
  for (const FailoverCandidate& candidate : set.candidates) {
    CF_CHECK_EQ(candidate.state, EligibilityState::Indeterminate);
  }
  const std::vector<EligibilityCause>& causes = set.candidates.front().causes;
  CF_CHECK(std::find(causes.begin(), causes.end(), EligibilityCause::CapacityEvidenceMissing) !=
           causes.end());
  CF_CHECK(set.selected.is_nil());

  // Creation is refused with the same evidence state.
  CF_CHECK_ERROR(harness->orchestrator->create_plan(
                     PlanRequest{RequestId::from_value(1), harness->now, PlanKind::Failover,
                                 SourceGroupId::from_value(1), SourceGroupId::nil(), "no evidence"}),
                 ErrorCode::EvidenceMissing);
}

CF_TEST(Eligibility, SharedReserveIsNeverCountedTwice) {
  cf_test::TempDir dir("eligibility-shared");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(),
                                 synthetic::two_group_plant(FailoverDomainId::from_value(9))));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  CF_ASSIGN_OR_FAIL(set, harness->orchestrator->evaluate_candidates(harness->now));

  CandidateKey primary_only;
  primary_only.groups.push_back(SourceGroupId::from_value(1));
  CandidateKey alternate_only;
  alternate_only.groups.push_back(SourceGroupId::from_value(2));
  CandidateKey united;
  united.groups = {SourceGroupId::from_value(1), SourceGroupId::from_value(2)};

  const FailoverCandidate* one = set.find(primary_only);
  const FailoverCandidate* two = set.find(alternate_only);
  const FailoverCandidate* both = set.find(united);
  CF_CHECK(one != nullptr);
  CF_CHECK(two != nullptr);
  CF_CHECK(both != nullptr);
  CF_CHECK_EQ(one->usable_capacity, Watts::from_watts(600000));
  CF_CHECK_EQ(two->usable_capacity, Watts::from_watts(550000));
  // The union counts the shared block once: 400000 + 200000 + 350000.
  CF_CHECK_EQ(both->usable_capacity, Watts::from_watts(950000));
  CF_CHECK(both->usable_capacity < *Watts::add(one->usable_capacity, two->usable_capacity));

  // Independent reference: brute-force the distinct block set of the union.
  std::set<std::uint64_t> blocks;
  const TopologyProjection& topology = harness->orchestrator->state().topology;
  for (const ReserveBlockDescriptor& descriptor : topology.blocks) {
    for (SourceGroupId group : united.groups) {
      if (std::find(descriptor.serving_groups.begin(), descriptor.serving_groups.end(), group) !=
          descriptor.serving_groups.end()) {
        blocks.insert(descriptor.block.value());
        break;
      }
    }
  }
  std::int64_t reference = 0;
  for (std::uint64_t block : blocks) {
    reference += harness->facility.block_capacity(ReserveBlockId::from_value(block)).value();
  }
  CF_CHECK_EQ(both->usable_capacity.value(), reference);

  std::set<std::uint64_t> allocated;
  for (const ReserveAllocation& allocation : both->allocations) {
    CF_CHECK(allocated.insert(allocation.block.value()).second);
  }
}

CF_TEST(Eligibility, IndependenceRequirementIsEnforced) {
  cf_test::TempDir dir("eligibility-independence");
  synthetic::SyntheticSpec spec = synthetic::two_group_plant(FailoverDomainId::from_value(10));
  spec.policy.minimum_independent_groups = 2;
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(dir.root(), spec));
  CF_CHECK_OK(harness->bootstrap(kEpoch));

  CF_ASSIGN_OR_FAIL(set, harness->orchestrator->evaluate_candidates(harness->now));
  CandidateKey single;
  single.groups.push_back(SourceGroupId::from_value(1));
  const FailoverCandidate* one = set.find(single);
  CF_CHECK(one != nullptr);
  CF_CHECK_EQ(one->state, EligibilityState::Ineligible);
  CF_CHECK(std::find(one->causes.begin(), one->causes.end(),
                     EligibilityCause::InsufficientIndependence) != one->causes.end());

  CandidateKey united;
  united.groups = {SourceGroupId::from_value(1), SourceGroupId::from_value(2)};
  const FailoverCandidate* both = set.find(united);
  CF_CHECK(both != nullptr);
  CF_CHECK(both->is_eligible());
}

CF_TEST(Eligibility, HeadroomShortfallIsIneligible) {
  cf_test::TempDir dir("eligibility-headroom");
  synthetic::SyntheticSpec spec = synthetic::two_group_plant(FailoverDomainId::from_value(11));
  spec.obligations.front().required_capacity = Watts::from_watts(900000);
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(dir.root(), spec));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  CF_ASSIGN_OR_FAIL(set, harness->orchestrator->evaluate_candidates(harness->now));
  CF_CHECK(!set.candidates.empty());
  for (const FailoverCandidate& candidate : set.candidates) {
    CF_CHECK_EQ(candidate.state, EligibilityState::Ineligible);
    CF_CHECK(std::find(candidate.causes.begin(), candidate.causes.end(),
                       EligibilityCause::InsufficientHeadroom) != candidate.causes.end());
  }
}

CF_TEST(Eligibility, AuthorityDenialRemovesEligibility) {
  cf_test::TempDir dir("eligibility-authority");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(),
                                 synthetic::two_group_plant(FailoverDomainId::from_value(12))));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  harness->now = Tick::from_value(3);
  CF_CHECK_OK(harness->publish_authority(AuthorityState::Denied, kEpoch));
  CF_ASSIGN_OR_FAIL(set, harness->orchestrator->evaluate_candidates(harness->now));
  for (const FailoverCandidate& candidate : set.candidates) {
    CF_CHECK_EQ(candidate.state, EligibilityState::Ineligible);
    CF_CHECK(std::find(candidate.causes.begin(), candidate.causes.end(),
                       EligibilityCause::AuthorityDenied) != candidate.causes.end());
  }
  CF_CHECK_ERROR(harness->orchestrator->create_plan(
                     PlanRequest{RequestId::from_value(1), harness->now, PlanKind::Failover,
                                 SourceGroupId::from_value(1), SourceGroupId::nil(), "denied"}),
                 ErrorCode::NoEligibleCandidate);
}

CF_TEST(Eligibility, ReorderedEvidenceIsRejected) {
  cf_test::TempDir dir("eligibility-reorder");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(),
                                 synthetic::two_group_plant(FailoverDomainId::from_value(13))));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  harness->now = Tick::from_value(10);
  CF_CHECK_OK(harness->publish_health(CoolingSourceId::from_value(1)));
  const SourceHealthEvidence stale = harness->facility.health_for(
      CoolingSourceId::from_value(1), harness->new_evidence(), Tick::from_value(2));
  CF_CHECK_ERROR(harness->orchestrator->record_health(stale), ErrorCode::EvidenceReordered);
}

CF_TEST(Eligibility, UnknownBlocksAreIndeterminate) {
  cf_test::TempDir dir("eligibility-unknown");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(),
                                 synthetic::two_group_plant(FailoverDomainId::from_value(14))));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  harness->facility.set_block(ReserveBlockId::from_value(3), AvailabilityState::Unknown,
                              Watts::zero());
  harness->now = Tick::from_value(4);
  CF_CHECK_OK(harness->publish_capacity());
  CF_ASSIGN_OR_FAIL(set, harness->orchestrator->evaluate_candidates(harness->now));
  CandidateKey alternate;
  alternate.groups.push_back(SourceGroupId::from_value(2));
  const FailoverCandidate* two = set.find(alternate);
  CF_CHECK(two != nullptr);
  CF_CHECK_EQ(two->state, EligibilityState::Indeterminate);
  CF_CHECK(std::find(two->causes.begin(), two->causes.end(), EligibilityCause::BlockUnknown) !=
           two->causes.end());
}

CF_TEST(Eligibility, ZeroAndAbsentAreDistinctStates) {
  cf_test::TempDir dir("eligibility-zero");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(),
                                 synthetic::two_group_plant(FailoverDomainId::from_value(15))));
  CF_CHECK_OK(harness->bootstrap(kEpoch));
  // Reported as exactly zero: a decisive negative, not missing evidence.
  harness->facility.set_block(ReserveBlockId::from_value(3), AvailabilityState::Zero,
                              Watts::zero());
  harness->now = Tick::from_value(2);
  CF_CHECK_OK(harness->publish_capacity());
  CF_ASSIGN_OR_FAIL(set, harness->orchestrator->evaluate_candidates(harness->now));
  CandidateKey alternate;
  alternate.groups.push_back(SourceGroupId::from_value(2));
  const FailoverCandidate* two = set.find(alternate);
  CF_CHECK(two != nullptr);
  CF_CHECK_EQ(two->state, EligibilityState::Ineligible);
  CF_CHECK(std::find(two->causes.begin(), two->causes.end(), EligibilityCause::BlockUnavailable) !=
           two->causes.end());
}
