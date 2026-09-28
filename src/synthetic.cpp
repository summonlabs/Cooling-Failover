#include "cooling_failover/synthetic.hpp"

#include <algorithm>
#include <utility>

namespace cooling_failover {
namespace synthetic {
namespace {

[[nodiscard]] std::uint64_t digest_word(const Digest& digest) noexcept {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(digest.bytes()[index]) << (8U * index);
  }
  return value == 0 ? 1 : value;
}

/// Observation identity is scoped by runtime, command, effect and sequence, so
/// two independent control runtimes can never mint the same identity.
[[nodiscard]] std::uint64_t observation_identity(OwnerSystem runtime, CommandId command,
                                                 EffectKind effect,
                                                 EffectSequence sequence) {
  Encoder encoder;
  encoder.u32(1);
  encoder.u8(static_cast<std::uint8_t>(runtime));
  encoder.id(command);
  encoder.u8(static_cast<std::uint8_t>(effect));
  encoder.counter(sequence);
  return digest_word(encoder.finish_digest());
}

}  // namespace

SyntheticFacility::SyntheticFacility(SyntheticSpec spec) : spec_(std::move(spec)) {
  for (const SyntheticBlock& block : spec_.blocks) {
    block_capacity_[block.block] = block.nominal;
  }
  for (const SyntheticGroup& group : spec_.groups) {
    mutation_.health[group.source] = HealthState::Healthy;
  }
  for (const SyntheticBlock& block : spec_.blocks) {
    mutation_.blocks[block.block] = std::make_pair(AvailabilityState::Available, block.nominal);
  }
}

TopologyProjection SyntheticFacility::topology(EvidenceId evidence, Tick now) const {
  TopologyProjection projection;
  projection.stamp.evidence = evidence;
  projection.stamp.kind = EvidenceKind::TopologyProjection;
  projection.stamp.owner = OwnerSystem::CoolingTopology;
  projection.stamp.observed_at = now;
  projection.stamp.reference = "synthetic://topology";
  projection.domain = spec_.domain;
  projection.generation = spec_.topology_generation;

  for (const SyntheticGroup& group : spec_.groups) {
    SourceGroupDescriptor descriptor;
    descriptor.group = group.group;
    descriptor.nominal_capacity = group.nominal;
    descriptor.designated_primary = group.designated_primary;
    projection.groups.push_back(descriptor);

    SourceDescriptor source;
    source.source = group.source;
    source.group = group.group;
    source.kind = group.kind;
    source.nominal_capacity = group.nominal;
    projection.sources.push_back(source);
  }
  std::sort(projection.groups.begin(), projection.groups.end(),
            [](const SourceGroupDescriptor& lhs, const SourceGroupDescriptor& rhs) {
              return lhs.group < rhs.group;
            });
  std::sort(projection.sources.begin(), projection.sources.end(),
            [](const SourceDescriptor& lhs, const SourceDescriptor& rhs) {
              return lhs.source < rhs.source;
            });

  for (const SyntheticBlock& block : spec_.blocks) {
    ReserveBlockDescriptor descriptor;
    descriptor.block = block.block;
    descriptor.nominal_capacity = block.nominal;
    descriptor.serving_groups = block.serving_groups;
    std::sort(descriptor.serving_groups.begin(), descriptor.serving_groups.end());
    descriptor.serving_groups.erase(
        std::unique(descriptor.serving_groups.begin(), descriptor.serving_groups.end()),
        descriptor.serving_groups.end());
    // The owning group is the lowest numbered serving group unless a group
    // declares the block as its own.
    descriptor.owner_group = descriptor.serving_groups.front();
    descriptor.owner_source = CoolingSourceId::nil();
    for (const SyntheticGroup& group : spec_.groups) {
      if (group.group == descriptor.owner_group) {
        descriptor.owner_source = group.source;
      }
    }
    for (const SyntheticGroup& group : spec_.groups) {
      if (std::find(group.owns_blocks.begin(), group.owns_blocks.end(), block.block) !=
          group.owns_blocks.end()) {
        descriptor.owner_group = group.group;
        descriptor.owner_source = group.source;
        break;
      }
    }
    projection.blocks.push_back(descriptor);
  }
  std::sort(projection.blocks.begin(), projection.blocks.end(),
            [](const ReserveBlockDescriptor& lhs, const ReserveBlockDescriptor& rhs) {
              return lhs.block < rhs.block;
            });
  return projection;
}

CapacityAvailabilityEvidence SyntheticFacility::capacity(EvidenceId evidence, Tick now) const {
  CapacityAvailabilityEvidence result;
  result.stamp.evidence = evidence;
  result.stamp.kind = EvidenceKind::CapacityAvailability;
  result.stamp.owner = OwnerSystem::CoolingCapacity;
  result.stamp.observed_at = now;
  result.stamp.reference = "synthetic://capacity";
  result.domain = spec_.domain;
  result.generation = spec_.capacity_generation;
  for (const SyntheticBlock& block : spec_.blocks) {
    BlockAvailability availability;
    availability.block = block.block;
    const auto it = mutation_.blocks.find(block.block);
    if (it != mutation_.blocks.end()) {
      availability.state = it->second.first;
      availability.available = it->second.second;
    } else {
      availability.state = AvailabilityState::Unknown;
      availability.available = Watts::zero();
    }
    availability.observed_at = now;
    result.blocks.push_back(availability);
  }
  std::sort(result.blocks.begin(), result.blocks.end(),
            [](const BlockAvailability& lhs, const BlockAvailability& rhs) {
              return lhs.block < rhs.block;
            });
  return result;
}

SourceHealthEvidence SyntheticFacility::health_for(CoolingSourceId source, EvidenceId evidence,
                                                   Tick now) const {
  SourceHealthEvidence result;
  result.stamp.evidence = evidence;
  result.stamp.kind = EvidenceKind::SourceHealth;
  result.stamp.owner = OwnerSystem::FacilityBms;
  result.stamp.observed_at = now;
  result.stamp.reference = "synthetic://health";
  result.source = source;
  const auto it = mutation_.health.find(source);
  result.state = it == mutation_.health.end() ? HealthState::Unknown : it->second;
  return result;
}

ServiceAuthorityEvidence SyntheticFacility::authority(EvidenceId evidence, Tick now) const {
  ServiceAuthorityEvidence result;
  result.stamp.evidence = evidence;
  result.stamp.kind = EvidenceKind::ServiceAuthority;
  result.stamp.owner = OwnerSystem::CoolingFailover;
  result.stamp.observed_at = now;
  result.stamp.reference = "synthetic://authority";
  result.domain = spec_.domain;
  result.epoch = mutation_.authority_epoch;
  result.state = mutation_.authority;
  return result;
}

ObligationSet SyntheticFacility::obligations(EvidenceId evidence, Tick now) const {
  ObligationSet result;
  result.stamp.evidence = evidence;
  result.stamp.kind = EvidenceKind::ProtectedObligationSet;
  result.stamp.owner = OwnerSystem::CoolingFailover;
  result.stamp.observed_at = now;
  result.stamp.reference = "synthetic://obligations";
  result.generation = 1;
  result.obligations = spec_.obligations;
  std::sort(result.obligations.begin(), result.obligations.end(),
            [](const ProtectedObligation& lhs, const ProtectedObligation& rhs) {
              return lhs.id < rhs.id;
            });
  return result;
}

void SyntheticFacility::set_health(CoolingSourceId source, HealthState state) {
  mutation_.health[source] = state;
}

void SyntheticFacility::set_block(ReserveBlockId block, AvailabilityState state, Watts available) {
  mutation_.blocks[block] = std::make_pair(state, available);
}

void SyntheticFacility::set_authority(AuthorityState state, ControlPlaneEpoch epoch) {
  mutation_.authority = state;
  mutation_.authority_epoch = epoch;
}

HealthState SyntheticFacility::health_of(CoolingSourceId source) const {
  const auto it = mutation_.health.find(source);
  return it == mutation_.health.end() ? HealthState::Unknown : it->second;
}

AvailabilityState SyntheticFacility::block_state(ReserveBlockId block) const {
  const auto it = mutation_.blocks.find(block);
  return it == mutation_.blocks.end() ? AvailabilityState::Unknown : it->second.first;
}

Watts SyntheticFacility::block_capacity(ReserveBlockId block) const {
  const auto it = mutation_.blocks.find(block);
  return it == mutation_.blocks.end() ? Watts::zero() : it->second.second;
}

void SyntheticFacility::fail_all_except(const std::vector<SourceGroupId>& survivors) {
  for (const SyntheticGroup& group : spec_.groups) {
    const bool keep =
        std::find(survivors.begin(), survivors.end(), group.group) != survivors.end();
    mutation_.health[group.source] = keep ? HealthState::Healthy : HealthState::Unavailable;
  }
}

// ---------------------------------------------------------------------------
// SyntheticControlRuntime
// ---------------------------------------------------------------------------

SyntheticControlRuntime::SyntheticControlRuntime(OwnerSystem runtime, std::string label)
    : runtime_(runtime), label_(std::move(label)) {}

const char* SyntheticControlRuntime::name() const noexcept { return label_.c_str(); }

Result<ControlResponse> SyntheticControlRuntime::issue(const ControlRequest& request) {
  ++issue_calls_;
  ControlResponse response;
  if (!behaviour_.reachable) {
    return Status::error(ErrorCode::AdapterUnavailable,
                         "synthetic control runtime is not reachable", "runtime", label_);
  }
  if (!behaviour_.acknowledges) {
    response.outcome = CommandOutcome::Refused;
    response.detail = "synthetic control runtime refused the request";
    return response;
  }
  // A repeated request for an already actuated command is acknowledged without
  // re-actuating: this is the runtime-side idempotency guard.
  const bool first_time = actuated_commands_.insert(request.command.value()).second;
  if (first_time) {
    ++actuations_;
  }
  response.outcome = CommandOutcome::Acknowledged;
  response.detail = first_time ? "synthetic runtime accepted and actuated"
                               : "synthetic runtime accepted a repeated request";
  return response;
}

Result<std::vector<EffectObservationRecord>> SyntheticControlRuntime::poll_effects(
    const ControlRequest& request) {
  ++poll_calls_;
  std::vector<EffectObservationRecord> observations;
  if (!behaviour_.reachable) {
    return Status::error(ErrorCode::AdapterUnavailable,
                         "synthetic control runtime is not reachable", "runtime", label_);
  }
  // Effects are a property of the plant, not of this process: a runtime that
  // restarted still reports the cooling state it observes.
  if (!behaviour_.emits_effects) {
    return observations;
  }
  for (EffectKind effect : request.requested_effects) {
    if (behaviour_.suppressed_effects.count(effect) != 0) {
      continue;
    }
    EffectObservationRecord observation;
    const EffectSequence sequence = EffectSequence::from_value(next_sequence_);
    observation.id = ObservationId::from_value(
        observation_identity(runtime_, request.command, effect, sequence));
    observation.command = request.command;
    observation.group = request.target;
    observation.effect = effect;
    observation.origin = behaviour_.acknowledgement_only
                             ? ObservationOrigin::ActuationControllerAck
                             : (runtime_ == OwnerSystem::AirflowControl
                                    ? ObservationOrigin::AirflowControl
                                    : ObservationOrigin::LiquidCoolingControl);
    observation.sequence = sequence;
    observation.topology = topology_generation_;
    observation.capacity = capacity_generation_;
    observation.epoch = request.epoch;
    observation.observed_at = request.issued_at;
    ++next_sequence_;
    observations.push_back(observation);
  }
  return observations;
}

void SyntheticControlRuntime::reset_counters() {
  issue_calls_ = 0;
  actuations_ = 0;
  poll_calls_ = 0;
  actuated_commands_.clear();
}

// ---------------------------------------------------------------------------
// Standard plant
// ---------------------------------------------------------------------------

SyntheticSpec two_group_plant(FailoverDomainId domain) {
  SyntheticSpec spec;
  spec.domain = domain;

  SyntheticGroup primary;
  primary.group = SourceGroupId::from_value(1);
  primary.source = CoolingSourceId::from_value(1);
  primary.kind = SourceKind::Chiller;
  primary.designated_primary = true;
  primary.nominal = Watts::from_watts(400000);
  primary.owns_blocks = {ReserveBlockId::from_value(1)};
  spec.groups.push_back(primary);

  SyntheticGroup alternate;
  alternate.group = SourceGroupId::from_value(2);
  alternate.source = CoolingSourceId::from_value(2);
  alternate.kind = SourceKind::Cdu;
  alternate.designated_primary = false;
  alternate.nominal = Watts::from_watts(350000);
  alternate.owns_blocks = {ReserveBlockId::from_value(2)};
  spec.groups.push_back(alternate);

  SyntheticBlock primary_block;
  primary_block.block = ReserveBlockId::from_value(1);
  primary_block.nominal = Watts::from_watts(400000);
  primary_block.serving_groups = {SourceGroupId::from_value(1)};
  spec.blocks.push_back(primary_block);

  // A shared thermal store serves both groups. A candidate that contains both
  // groups must count it exactly once.
  SyntheticBlock shared_block;
  shared_block.block = ReserveBlockId::from_value(2);
  shared_block.nominal = Watts::from_watts(200000);
  shared_block.serving_groups = {SourceGroupId::from_value(1), SourceGroupId::from_value(2)};
  spec.blocks.push_back(shared_block);

  SyntheticBlock alternate_block;
  alternate_block.block = ReserveBlockId::from_value(3);
  alternate_block.nominal = Watts::from_watts(350000);
  alternate_block.serving_groups = {SourceGroupId::from_value(2)};
  spec.blocks.push_back(alternate_block);

  ProtectedObligation obligation;
  obligation.id = ObligationId::from_value(1);
  obligation.required_capacity = Watts::from_watts(300000);
  obligation.required_independent_groups = 1;
  obligation.criticality = Criticality::Critical;
  obligation.reference = "synthetic://hall-a";
  spec.obligations.push_back(obligation);

  spec.policy.generation = PolicyGeneration::from_value(1);
  spec.policy.selection = SelectionMode::DeterministicRanked;
  spec.policy.rank_order = {RankCriterion::DescendingUsableCapacity,
                            RankCriterion::FewestGroups,
                            RankCriterion::AscendingGroupKey};
  spec.policy.minimum_independent_groups = 1;
  spec.policy.max_groups_per_candidate = 2;
  spec.policy.headroom_margin = *BasisPoints::from_value(1000);  // 10 percent
  spec.policy.recovery_consecutive_healthy = 3;
  spec.policy.recovery_dwell_ticks = 30;
  spec.policy.max_evidence_age_ticks = 0;
  spec.policy.oscillation_window_ticks = 60;
  spec.policy.max_transitions_per_window = 3;
  spec.policy.max_plan_steps = 8;
  spec.policy.stamp.evidence = EvidenceId::from_value(500);
  spec.policy.stamp.kind = EvidenceKind::RedundancyPolicy;
  spec.policy.stamp.owner = OwnerSystem::CoolingFailover;
  spec.policy.stamp.observed_at = Tick::from_value(0);
  spec.policy.stamp.reference = "synthetic://policy";
  return spec;
}

}  // namespace synthetic
}  // namespace cooling_failover
