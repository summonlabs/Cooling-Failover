// Resource bounds, path safety and arithmetic limits.
#include "harness.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

using namespace cooling_failover;

namespace {

const ControlPlaneEpoch kEpoch = ControlPlaneEpoch::from_value(1);

}  // namespace

CF_TEST(Bounds, StoreRootPathSafety) {
  const std::vector<std::string> rejected = {
      "",
      "relative/../escape",
      "C:\\temp\\..\\escape",
      "\\\\?\\C:\\temp\\device",
      "\\\\.\\pipe\\cooling",
      "C:\\temp\\name:stream",
      "C:\\temp\\CON",
      "C:\\temp\\nul",
      "C:\\temp\\trailing.",
  };
  for (const std::string& path : rejected) {
    const Result<std::filesystem::path> prepared = prepare_store_root(path);
    CF_CHECK_MSG(!prepared.has_value(), "path was accepted: " + path);
    CF_CHECK(prepared.status().code() == ErrorCode::InvalidPath ||
             prepared.status().code() == ErrorCode::PathAmbiguous);
  }

  std::string long_component = "C:\\";
  long_component += std::string(300, 'a');
  CF_CHECK_ERROR(prepare_store_root(long_component), ErrorCode::PathTooLong);

  std::string embedded_nul = "C:\\temp\\";
  embedded_nul.push_back('\0');
  embedded_nul += "x";
  CF_CHECK_ERROR(prepare_store_root(embedded_nul), ErrorCode::InvalidPath);

  cf_test::TempDir dir("bounds-path");
  CF_ASSIGN_OR_FAIL(prepared, prepare_store_root(dir.path() / "nested" / "store"));
  CF_CHECK(std::filesystem::is_directory(prepared));
  // The canonical root of the same logical store is stable across calls: a
  // doubled separator names the same directory.
  CF_ASSIGN_OR_FAIL(again, prepare_store_root(dir.path() / "nested//store"));
  CF_CHECK_EQ(prepared, again);
}

CF_TEST(Bounds, TopologyAndPolicyLimits) {
  cf_test::TempDir dir("bounds-limits");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(
                                                 FailoverDomainId::from_value(80))));
  CF_CHECK_OK(harness->orchestrator->register_domain(harness->now));
  CF_CHECK_OK(harness->publish_topology());

  synthetic::SyntheticSpec spec = synthetic::two_group_plant(FailoverDomainId::from_value(80));
  spec.policy.max_groups_per_candidate = kMaxGroupsPerCandidate + 1;
  CF_CHECK_ERROR(harness->orchestrator->install_policy(spec.policy), ErrorCode::OutOfRange);

  spec.policy.max_groups_per_candidate = 1;
  spec.policy.minimum_independent_groups = 0;
  CF_CHECK_ERROR(harness->orchestrator->install_policy(spec.policy), ErrorCode::OutOfRange);

  spec.policy.minimum_independent_groups = 1;
  spec.policy.selection = SelectionMode::DeterministicRanked;
  spec.policy.rank_order.clear();
  CF_CHECK_ERROR(harness->orchestrator->install_policy(spec.policy), ErrorCode::InvalidArgument);

  TopologyProjection oversized = harness->facility.topology(harness->new_evidence(), harness->now);
  oversized.groups.clear();
  for (std::uint32_t index = 0; index < kMaxGroupsPerDomain + 1; ++index) {
    SourceGroupDescriptor group;
    group.group = SourceGroupId::from_value(index + 1);
    oversized.groups.push_back(group);
  }
  oversized.sources.clear();
  oversized.blocks.clear();
  CF_CHECK_ERROR(harness->orchestrator->import_topology(oversized), ErrorCode::BoundsExceeded);
}

CF_TEST(Bounds, CapacityArithmeticOverflowIsReported) {
  Watts accumulator = Watts::from_watts(Watts::max_watts() - 1);
  CF_CHECK_OK(Watts::add(accumulator, Watts::from_watts(1)));
  CF_CHECK_ERROR(Watts::add(accumulator, Watts::from_watts(2)), ErrorCode::Overflow);
  const std::int64_t values[] = {Watts::max_watts(), Watts::max_watts()};
  CF_CHECK_ERROR(Watts::sum(values, 2), ErrorCode::Overflow);
  CF_CHECK_EQ(*Watts::scale_up(Watts::from_watts(Watts::max_watts()),
                               *BasisPoints::from_value(10000)),
              Watts::from_watts(Watts::max_watts()));
}

CF_TEST(Bounds, ObservationRetentionIsBounded) {
  cf_test::TempDir dir("bounds-retention");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(
                                                 FailoverDomainId::from_value(81))));
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

  // Feeding far more observations than the retention bound must not grow state
  // without limit; the attempt is already verified so this only exercises the
  // bounded store.
  const CommandSpec& command = plan.commands.front();
  EffectSequence sequence = EffectSequence::from_value(10000000);
  for (std::uint32_t index = 0; index < kMaxRetainedObservations + 64; ++index) {
    EffectObservationRecord observation;
    observation.id = ObservationId::from_value(500000 + index);
    observation.command = command.id;
    observation.group = command.target;
    observation.effect = command.required_effects.front();
    observation.origin = ObservationOrigin::LiquidCoolingControl;
    observation.sequence = sequence;
    observation.topology = plan.topology;
    observation.capacity = plan.capacity;
    observation.epoch = plan.epoch;
    observation.observed_at = Tick::from_value(6);
    CF_CHECK_OK(harness->orchestrator->submit_observation(observation));
    CF_ASSIGN_OR_FAIL(next, sequence.next());
    sequence = next;
  }
  CF_CHECK(harness->orchestrator->state().observations.size() <= kMaxRetainedObservations);
}

CF_TEST(Bounds, MalformedTopologyIsRefusedDeterministically) {
  cf_test::TempDir dir("bounds-topology");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(
                                                 FailoverDomainId::from_value(82))));
  CF_CHECK_OK(harness->orchestrator->register_domain(harness->now));

  TopologyProjection projection = harness->facility.topology(harness->new_evidence(), harness->now);
  TopologyProjection unsorted = projection;
  std::swap(unsorted.groups[0], unsorted.groups[1]);
  CF_CHECK_ERROR(harness->orchestrator->import_topology(unsorted), ErrorCode::DuplicateIdentity);

  TopologyProjection duplicated = projection;
  duplicated.groups[1] = duplicated.groups[0];
  CF_CHECK_ERROR(harness->orchestrator->import_topology(duplicated),
                 ErrorCode::DuplicateIdentity);

  TopologyProjection foreign = projection;
  foreign.domain = FailoverDomainId::from_value(999);
  CF_CHECK_ERROR(harness->orchestrator->import_topology(foreign), ErrorCode::InvalidArgument);

  TopologyProjection unset_generation = projection;
  unset_generation.generation = TopologyGeneration::unset();
  CF_CHECK_ERROR(harness->orchestrator->import_topology(unset_generation),
                 ErrorCode::InvalidArgument);

  CF_CHECK_OK(harness->orchestrator->import_topology(projection));
  // An older generation is a stale regression and is refused.
  TopologyProjection older = projection;
  older.generation = TopologyGeneration::unset();
  CF_CHECK_ERROR(harness->orchestrator->import_topology(older), ErrorCode::InvalidArgument);
  TopologyProjection same = projection;
  CF_CHECK_OK(harness->orchestrator->import_topology(same));  // exact replay is idempotent
}

CF_TEST(Bounds, CapacityEvidenceForUnknownBlocksIsRefused) {
  cf_test::TempDir dir("bounds-capacity");
  CF_ASSIGN_OR_FAIL(harness, cf_test::DomainHarness::open(
                                 dir.root(), synthetic::two_group_plant(
                                                 FailoverDomainId::from_value(83))));
  CF_CHECK_OK(harness->orchestrator->register_domain(harness->now));
  CF_CHECK_OK(harness->publish_topology());
  CapacityAvailabilityEvidence evidence =
      harness->facility.capacity(harness->new_evidence(), harness->now);
  BlockAvailability unknown;
  unknown.block = ReserveBlockId::from_value(4096);
  unknown.state = AvailabilityState::Available;
  unknown.available = Watts::from_watts(1000);
  unknown.observed_at = harness->now;
  evidence.blocks.push_back(unknown);
  std::sort(evidence.blocks.begin(), evidence.blocks.end(),
            [](const BlockAvailability& lhs, const BlockAvailability& rhs) {
              return lhs.block < rhs.block;
            });
  CF_CHECK_ERROR(harness->orchestrator->record_capacity(evidence), ErrorCode::InvalidArgument);

  CapacityAvailabilityEvidence inconsistent =
      harness->facility.capacity(harness->new_evidence(), harness->now);
  inconsistent.blocks.front().state = AvailabilityState::Available;
  inconsistent.blocks.front().available = Watts::zero();
  CF_CHECK_ERROR(harness->orchestrator->record_capacity(inconsistent),
                 ErrorCode::InvalidArgument);
}
