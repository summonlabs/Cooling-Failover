// Semantic and byte-for-byte determinism, cross-checked against a reference model.
#include "harness.hpp"

#include <algorithm>
#include <random>
#include <set>
#include <vector>

using namespace cooling_failover;

namespace {

const ControlPlaneEpoch kEpoch = ControlPlaneEpoch::from_value(1);

/// Independent reference model: an arrangement is eligible only when the union
/// of its distinct available blocks covers the margin-adjusted requirement, the
/// independence requirement holds, and every consumed source reports Healthy.
struct ReferenceOutcome {
  bool eligible{false};
  std::int64_t usable{0};
};

ReferenceOutcome reference_evaluate(const TopologyProjection& topology,
                                    const CapacityAvailabilityEvidence& capacity,
                                    const std::map<CoolingSourceId, HealthState>& health,
                                    const ObligationSet& obligations,
                                    const RedundancyPolicy& policy,
                                    const std::vector<SourceGroupId>& arrangement) {
  ReferenceOutcome outcome;
  std::set<std::uint64_t> blocks;
  for (const ReserveBlockDescriptor& descriptor : topology.blocks) {
    for (SourceGroupId group : arrangement) {
      if (std::find(descriptor.serving_groups.begin(), descriptor.serving_groups.end(), group) !=
          descriptor.serving_groups.end()) {
        blocks.insert(descriptor.block.value());
        break;
      }
    }
  }
  for (std::uint64_t block : blocks) {
    const ReserveBlockDescriptor* descriptor = nullptr;
    for (const ReserveBlockDescriptor& candidate : topology.blocks) {
      if (candidate.block.value() == block) {
        descriptor = &candidate;
      }
    }
    if (descriptor == nullptr) {
      continue;
    }
    const auto source_health = health.find(descriptor->owner_source);
    if (source_health == health.end() || source_health->second != HealthState::Healthy) {
      continue;
    }
    const BlockAvailability* availability = capacity.find(ReserveBlockId::from_value(block));
    if (availability == nullptr || availability->state != AvailabilityState::Available) {
      continue;
    }
    outcome.usable += availability->available.value();
  }
  std::int64_t required = 0;
  for (const ProtectedObligation& obligation : obligations.obligations) {
    required += obligation.required_capacity.value();
  }
  const std::int64_t margin = (required * policy.headroom_margin.value() + 9999) / 10000;
  const std::int64_t required_with_margin = required + margin;
  bool independence = arrangement.size() >= policy.minimum_independent_groups;
  for (const ProtectedObligation& obligation : obligations.obligations) {
    if (arrangement.size() < obligation.required_independent_groups) {
      independence = false;
    }
  }
  outcome.eligible = independence && outcome.usable >= required_with_margin;
  return outcome;
}

}  // namespace

CF_TEST(Determinism, CandidateSetIsSemanticallyDeterministic) {
  cf_test::TempDir dir("determinism-candidates");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(
                                                 FailoverDomainId::from_value(70))));
  CF_CHECK_OK(harness->bootstrap(kEpoch));

  CF_ASSIGN_OR_FAIL(first, harness->orchestrator->evaluate_candidates(Tick::from_value(3)));
  CF_ASSIGN_OR_FAIL(second, harness->orchestrator->evaluate_candidates(Tick::from_value(3)));
  CF_CHECK_EQ(first.candidates.size(), second.candidates.size());
  for (std::size_t index = 0; index < first.candidates.size(); ++index) {
    CF_CHECK_EQ(first.candidates[index].id, second.candidates[index].id);
    CF_CHECK_EQ(static_cast<int>(first.candidates[index].state),
                static_cast<int>(second.candidates[index].state));
    CF_CHECK_EQ(first.candidates[index].usable_capacity.value(),
                second.candidates[index].usable_capacity.value());
    CF_CHECK_EQ(first.candidates[index].rank, second.candidates[index].rank);
  }
  CF_CHECK(!first.selected.is_nil());
  CF_CHECK_EQ(first.selected, second.selected);
}

CF_TEST(Determinism, CanonicalStateIsByteForByteDeterministic) {
  auto build = [](const std::filesystem::path& root, FailoverDomainId domain) {
    cooling_failover::Result<std::unique_ptr<cf_test::DomainHarness>> opened =
        cf_test::DomainHarness::open(root, synthetic::two_group_plant(domain));
    CF_CHECK(opened.has_value());
    std::unique_ptr<cf_test::DomainHarness> harness = std::move(*opened);
    CF_CHECK_OK(harness->bootstrap(kEpoch));
    harness->now = Tick::from_value(5);
    harness->facility.fail_all_except({SourceGroupId::from_value(2)});
    CF_CHECK_OK(harness->publish_health(CoolingSourceId::from_value(1)));
    CF_CHECK_OK(harness->publish_health(CoolingSourceId::from_value(2)));
    PlanRequest request;
    request.request = RequestId::from_value(1);
    request.now = harness->now;
    request.kind = PlanKind::Failover;
    request.incumbent = SourceGroupId::from_value(1);
    request.reason = "primary unavailable";
    CF_ASSIGN_OR_FAIL(plan, harness->orchestrator->create_plan(request));
    CF_CHECK_OK(harness->orchestrator->execute_plan(plan.id, harness->now));
    return harness->orchestrator->canonical_state();
  };

  cf_test::TempDir first_dir("determinism-state-a");
  cf_test::TempDir second_dir("determinism-state-b");
  const std::vector<std::uint8_t> first = build(first_dir.root(), FailoverDomainId::from_value(71));
  const std::vector<std::uint8_t> second =
      build(second_dir.root(), FailoverDomainId::from_value(71));
  CF_CHECK(!first.empty());
  CF_CHECK_EQ(first.size(), second.size());
  CF_CHECK(first == second);
}

CF_TEST(Determinism, PlanIdentityAndShapeAreDeterministic) {
  auto plan_identity = [](const std::filesystem::path& root, FailoverDomainId domain) {
    cooling_failover::Result<std::unique_ptr<cf_test::DomainHarness>> opened =
        cf_test::DomainHarness::open(root, synthetic::two_group_plant(domain));
    CF_CHECK(opened.has_value());
    std::unique_ptr<cf_test::DomainHarness> harness = std::move(*opened);
    CF_CHECK_OK(harness->bootstrap(kEpoch));
    harness->now = Tick::from_value(9);
    harness->facility.fail_all_except({SourceGroupId::from_value(2)});
    CF_CHECK_OK(harness->publish_health(CoolingSourceId::from_value(1)));
    CF_CHECK_OK(harness->publish_health(CoolingSourceId::from_value(2)));
    PlanRequest request;
    request.request = RequestId::from_value(1);
    request.now = harness->now;
    request.kind = PlanKind::Failover;
    request.incumbent = SourceGroupId::from_value(1);
    request.reason = "primary unavailable";
    CF_ASSIGN_OR_FAIL(plan, harness->orchestrator->create_plan(request));
    std::vector<std::uint64_t> shape;
    shape.push_back(plan.id.value());
    shape.push_back(plan.binding_digest().bytes()[0]);
    for (const PlanStep& step : plan.steps) {
      shape.push_back(step.ordinal);
      shape.push_back(step.target.value());
    }
    for (const CommandSpec& command : plan.commands) {
      shape.push_back(command.id.value());
      shape.push_back(command.key.value());
    }
    return shape;
  };

  cf_test::TempDir first_dir("determinism-plan-a");
  cf_test::TempDir second_dir("determinism-plan-b");
  const std::vector<std::uint64_t> first =
      plan_identity(first_dir.root(), FailoverDomainId::from_value(72));
  const std::vector<std::uint64_t> second =
      plan_identity(second_dir.root(), FailoverDomainId::from_value(72));
  CF_CHECK(!first.empty());
  CF_CHECK(first == second);
}

CF_TEST(Determinism, ProductionEligibilityMatchesTheReferenceModel) {
  cf_test::TempDir dir("determinism-reference");
  std::mt19937_64 engine(20260214ULL);
  std::uniform_int_distribution<int> health_roll(0, 4);
  std::uniform_int_distribution<int> block_roll(0, 5);
  std::uniform_int_distribution<std::int64_t> capacity_roll(0, 400000);

  for (int iteration = 0; iteration < 12; ++iteration) {
    const FailoverDomainId domain =
        FailoverDomainId::from_value(1000 + static_cast<std::uint64_t>(iteration));
    synthetic::SyntheticSpec spec = synthetic::two_group_plant(domain);
    cf_test::TempDir iteration_dir("determinism-iteration");
    CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(iteration_dir.root(), spec));
    CF_CHECK_OK(harness->bootstrap(kEpoch));

    for (const synthetic::SyntheticGroup& group : spec.groups) {
      const int roll = health_roll(engine);
      const HealthState state = roll == 0   ? HealthState::Healthy
                                : roll == 1 ? HealthState::Degraded
                                : roll == 2 ? HealthState::Unavailable
                                : roll == 3 ? HealthState::Unsupported
                                            : HealthState::Unknown;
      harness->facility.set_health(group.source, state);
      harness->now = Tick::from_value(static_cast<std::uint64_t>(iteration + 1));
      const cooling_failover::Result<void> recorded = harness->publish_health(group.source);
      if (!recorded.has_value()) {
        // A source may have been removed from the topology; skip it.
        continue;
      }
    }
    for (const synthetic::SyntheticBlock& block : spec.blocks) {
      const int roll = block_roll(engine);
      const AvailabilityState state = roll == 0   ? AvailabilityState::Available
                                      : roll == 1 ? AvailabilityState::Zero
                                      : roll == 2 ? AvailabilityState::Unavailable
                                      : roll == 3 ? AvailabilityState::Unknown
                                      : roll == 4 ? AvailabilityState::Unsupported
                                                  : AvailabilityState::Indeterminate;
      const Watts capacity =
          state == AvailabilityState::Available
              ? Watts::from_watts(capacity_roll(engine) + 1)
              : Watts::zero();
      harness->facility.set_block(block.block, state, capacity);
    }
    harness->now = Tick::from_value(50);
    CF_CHECK_OK(harness->publish_capacity());

    CF_ASSIGN_OR_FAIL(set, harness->orchestrator->evaluate_candidates(harness->now));
    const DomainState& state = harness->orchestrator->state();
    for (const FailoverCandidate& candidate : set.candidates) {
      const bool decisive = candidate.state == EligibilityState::Eligible ||
                            candidate.state == EligibilityState::Ineligible;
      if (!decisive) {
        continue;  // indeterminate outcomes are covered by the eligibility suite
      }
      const ReferenceOutcome reference =
          reference_evaluate(state.topology, state.capacity, [&] {
            std::map<CoolingSourceId, HealthState> health;
            for (const auto& entry : state.health) {
              health[entry.first] = entry.second.state;
            }
            return health;
          }(), state.obligations, state.policy, candidate.key.groups);
      CF_CHECK_MSG(reference.eligible == (candidate.state == EligibilityState::Eligible),
                   "production and reference disagree for arrangement size " +
                       std::to_string(candidate.key.groups.size()));
      if (reference.eligible) {
        CF_CHECK_EQ(candidate.usable_capacity.value(), reference.usable);
      }
    }
  }
}
