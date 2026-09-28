#include "cooling_failover/model.hpp"

#include <algorithm>

namespace cooling_failover {

bool is_evidence_bearing_origin(OwnerSystem owner) noexcept {
  return owner != OwnerSystem::ActuationControllerAck && owner != OwnerSystem::CoolingFailover;
}

OwnerSystem actuation_runtime_for(SourceKind kind) noexcept {
  switch (kind) {
    case SourceKind::Crac:
    case SourceKind::Crah:
    case SourceKind::Economizer:
      return OwnerSystem::AirflowControl;
    case SourceKind::Chiller:
    case SourceKind::Cdu:
    case SourceKind::PrimaryPump:
    case SourceKind::SecondaryLoop:
    case SourceKind::ThermalStore:
      return OwnerSystem::LiquidCoolingControl;
  }
  return OwnerSystem::LiquidCoolingControl;
}

const char* to_string(HealthState state) noexcept {
  switch (state) {
    case HealthState::Unknown: return "Unknown";
    case HealthState::Healthy: return "Healthy";
    case HealthState::Degraded: return "Degraded";
    case HealthState::Unavailable: return "Unavailable";
    case HealthState::Unsupported: return "Unsupported";
  }
  return "Invalid";
}

const char* to_string(OwnerSystem owner) noexcept {
  switch (owner) {
    case OwnerSystem::CoolingTopology: return "CoolingTopology";
    case OwnerSystem::CoolingCapacity: return "CoolingCapacity";
    case OwnerSystem::AirflowControl: return "AirflowControl";
    case OwnerSystem::LiquidCoolingControl: return "LiquidCoolingControl";
    case OwnerSystem::FacilityBms: return "FacilityBms";
    case OwnerSystem::PowerControl: return "PowerControl";
    case OwnerSystem::ThermalZone: return "ThermalZone";
    case OwnerSystem::CoolingFailover: return "CoolingFailover";
    case OwnerSystem::ActuationControllerAck: return "ActuationControllerAck";
  }
  return "Invalid";
}

const char* to_string(EvidenceKind kind) noexcept {
  switch (kind) {
    case EvidenceKind::TopologyProjection: return "TopologyProjection";
    case EvidenceKind::CapacityAvailability: return "CapacityAvailability";
    case EvidenceKind::SourceHealth: return "SourceHealth";
    case EvidenceKind::ServiceAuthority: return "ServiceAuthority";
    case EvidenceKind::RedundancyPolicy: return "RedundancyPolicy";
    case EvidenceKind::ProtectedObligationSet: return "ProtectedObligationSet";
    case EvidenceKind::CommandAcknowledgement: return "CommandAcknowledgement";
    case EvidenceKind::EffectObservation: return "EffectObservation";
  }
  return "Invalid";
}

const char* to_string(SourceKind kind) noexcept {
  switch (kind) {
    case SourceKind::Chiller: return "Chiller";
    case SourceKind::Cdu: return "Cdu";
    case SourceKind::Crac: return "Crac";
    case SourceKind::Crah: return "Crah";
    case SourceKind::PrimaryPump: return "PrimaryPump";
    case SourceKind::SecondaryLoop: return "SecondaryLoop";
    case SourceKind::Economizer: return "Economizer";
    case SourceKind::ThermalStore: return "ThermalStore";
  }
  return "Invalid";
}

const char* to_string(AuthorityState state) noexcept {
  switch (state) {
    case AuthorityState::Unknown: return "Unknown";
    case AuthorityState::Granted: return "Granted";
    case AuthorityState::Denied: return "Denied";
    case AuthorityState::Superseded: return "Superseded";
  }
  return "Invalid";
}

const char* to_string(AvailabilityState state) noexcept {
  switch (state) {
    case AvailabilityState::Available: return "Available";
    case AvailabilityState::Zero: return "Zero";
    case AvailabilityState::Unavailable: return "Unavailable";
    case AvailabilityState::Unknown: return "Unknown";
    case AvailabilityState::Unsupported: return "Unsupported";
    case AvailabilityState::Indeterminate: return "Indeterminate";
  }
  return "Invalid";
}

const char* to_string(RankCriterion criterion) noexcept {
  switch (criterion) {
    case RankCriterion::AscendingGroupKey: return "AscendingGroupKey";
    case RankCriterion::DescendingUsableCapacity: return "DescendingUsableCapacity";
    case RankCriterion::DescendingHeadroom: return "DescendingHeadroom";
    case RankCriterion::FewestGroups: return "FewestGroups";
    case RankCriterion::MostGroups: return "MostGroups";
    case RankCriterion::OwnerGroupFirst: return "OwnerGroupFirst";
  }
  return "Invalid";
}

const char* to_string(SelectionMode mode) noexcept {
  switch (mode) {
    case SelectionMode::NotDefined: return "NotDefined";
    case SelectionMode::DeterministicRanked: return "DeterministicRanked";
    case SelectionMode::ManualOnly: return "ManualOnly";
  }
  return "Invalid";
}

const SourceGroupDescriptor* TopologyProjection::find_group(SourceGroupId group) const noexcept {
  const auto it = std::lower_bound(
      groups.begin(), groups.end(), group,
      [](const SourceGroupDescriptor& entry, SourceGroupId wanted) { return entry.group < wanted; });
  if (it == groups.end() || !(it->group == group)) {
    return nullptr;
  }
  return &*it;
}

bool TopologyProjection::contains_group(SourceGroupId group) const noexcept {
  return find_group(group) != nullptr;
}

std::vector<CoolingSourceId> TopologyProjection::members_of(SourceGroupId group) const {
  std::vector<CoolingSourceId> members;
  for (const SourceDescriptor& source : sources) {
    if (source.group == group) {
      members.push_back(source.source);
    }
  }
  return members;
}

std::vector<ReserveBlockId> TopologyProjection::blocks_serving(SourceGroupId group) const {
  std::vector<ReserveBlockId> result;
  for (const ReserveBlockDescriptor& block : blocks) {
    if (std::find(block.serving_groups.begin(), block.serving_groups.end(), group) !=
        block.serving_groups.end()) {
      result.push_back(block.block);
    }
  }
  return result;
}

const BlockAvailability* CapacityAvailabilityEvidence::find(ReserveBlockId block) const noexcept {
  const auto it = std::lower_bound(
      blocks.begin(), blocks.end(), block,
      [](const BlockAvailability& entry, ReserveBlockId wanted) { return entry.block < wanted; });
  if (it == blocks.end() || !(it->block == block)) {
    return nullptr;
  }
  return &*it;
}

std::uint64_t ObligationSet::total_required_capacity() const noexcept {
  std::uint64_t total = 0;
  for (const ProtectedObligation& obligation : obligations) {
    const std::int64_t value = obligation.required_capacity.value();
    if (value <= 0) {
      continue;
    }
    const std::uint64_t positive = static_cast<std::uint64_t>(value);
    if (total > UINT64_MAX - positive) {
      return UINT64_MAX;
    }
    total += positive;
  }
  return total;
}

}  // namespace cooling_failover
