#include "cooling_failover/eligibility.hpp"

#include "cooling_failover/orchestrator.hpp"
#include "cooling_failover/state.hpp"

#include <algorithm>
#include <set>

namespace cooling_failover {
namespace {

[[nodiscard]] bool contains_cause(const std::vector<EligibilityCause>& causes,
                                  EligibilityCause cause) {
  return std::find(causes.begin(), causes.end(), cause) != causes.end();
}

void add_cause(std::vector<EligibilityCause>& causes, EligibilityCause cause) {
  if (cause == EligibilityCause::None || contains_cause(causes, cause)) {
    return;
  }
  causes.push_back(cause);
}

[[nodiscard]] bool any_indeterminate(const std::vector<EligibilityCause>& causes) {
  for (EligibilityCause cause : causes) {
    if (cause_is_indeterminate(cause)) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool any_ineligible(const std::vector<EligibilityCause>& causes) {
  for (EligibilityCause cause : causes) {
    if (!cause_is_indeterminate(cause)) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] const ReserveBlockDescriptor* find_block(const TopologyProjection& topology,
                                                       ReserveBlockId block) {
  const auto it = std::lower_bound(
      topology.blocks.begin(), topology.blocks.end(), block,
      [](const ReserveBlockDescriptor& entry, ReserveBlockId wanted) { return entry.block < wanted; });
  if (it == topology.blocks.end() || !(it->block == block)) {
    return nullptr;
  }
  return &*it;
}

/// Every source the arrangement or the incumbent depends on: the members of the
/// arrangement's groups plus the owner of every reserve block serving them.
[[nodiscard]] std::vector<CoolingSourceId> consumed_sources(const DomainState& state,
                                                            const std::vector<SourceGroupId>& key,
                                                            SourceGroupId incumbent) {
  std::set<CoolingSourceId> sources;
  if (state.has_topology) {
    std::vector<SourceGroupId> groups = key;
    if (!incumbent.is_nil()) {
      groups.push_back(incumbent);
    }
    for (SourceGroupId group : groups) {
      for (CoolingSourceId member : state.topology.members_of(group)) {
        sources.insert(member);
      }
      for (ReserveBlockId block : state.topology.blocks_serving(group)) {
        if (const ReserveBlockDescriptor* descriptor = find_block(state.topology, block)) {
          sources.insert(descriptor->owner_source);
        }
      }
    }
  }
  return std::vector<CoolingSourceId>(sources.begin(), sources.end());
}

[[nodiscard]] bool group_in(const std::vector<SourceGroupId>& groups, SourceGroupId group) {
  return std::find(groups.begin(), groups.end(), group) != groups.end();
}

struct AgeCheck {
  bool enabled{false};
  std::uint64_t max_age{0};
  Tick now{};

  /// Returns true when the observation is too old or is dated in the future.
  [[nodiscard]] bool stale(Tick observed_at) const noexcept {
    if (!enabled) {
      return false;
    }
    return now.elapsed_since(observed_at) > max_age;
  }
  [[nodiscard]] bool from_future(Tick observed_at) const noexcept {
    return observed_at > now;
  }
};

}  // namespace

const char* to_string(EligibilityState state) noexcept {
  switch (state) {
    case EligibilityState::Eligible: return "Eligible";
    case EligibilityState::Ineligible: return "Ineligible";
    case EligibilityState::Indeterminate: return "Indeterminate";
  }
  return "Invalid";
}

const char* to_string(EligibilityCause cause) noexcept {
  switch (cause) {
    case EligibilityCause::None: return "None";
    case EligibilityCause::TopologyNotImported: return "TopologyNotImported";
    case EligibilityCause::PolicyMissing: return "PolicyMissing";
    case EligibilityCause::ObligationsMissing: return "ObligationsMissing";
    case EligibilityCause::AuthorityMissing: return "AuthorityMissing";
    case EligibilityCause::AuthorityDenied: return "AuthorityDenied";
    case EligibilityCause::AuthorityUnknown: return "AuthorityUnknown";
    case EligibilityCause::AuthoritySuperseded: return "AuthoritySuperseded";
    case EligibilityCause::EpochNotEstablished: return "EpochNotEstablished";
    case EligibilityCause::EpochMismatch: return "EpochMismatch";
    case EligibilityCause::HealthMissing: return "HealthMissing";
    case EligibilityCause::HealthUnsupported: return "HealthUnsupported";
    case EligibilityCause::HealthDegraded: return "HealthDegraded";
    case EligibilityCause::HealthUnavailable: return "HealthUnavailable";
    case EligibilityCause::NoUsableReserve: return "NoUsableReserve";
    case EligibilityCause::CapacityEvidenceMissing: return "CapacityEvidenceMissing";
    case EligibilityCause::CapacityEvidenceStale: return "CapacityEvidenceStale";
    case EligibilityCause::BlockUnavailable: return "BlockUnavailable";
    case EligibilityCause::BlockUnknown: return "BlockUnknown";
    case EligibilityCause::InsufficientHeadroom: return "InsufficientHeadroom";
    case EligibilityCause::InsufficientIndependence: return "InsufficientIndependence";
    case EligibilityCause::ObligationUnprotected: return "ObligationUnprotected";
    case EligibilityCause::EvidenceTooOld: return "EvidenceTooOld";
    case EligibilityCause::GroupNotInTopology: return "GroupNotInTopology";
    case EligibilityCause::GroupEmpty: return "GroupEmpty";
    case EligibilityCause::ArrangementTooLarge: return "ArrangementTooLarge";
    case EligibilityCause::EmptyArrangement: return "EmptyArrangement";
    case EligibilityCause::EvidenceFromFuture: return "EvidenceFromFuture";
  }
  return "Invalid";
}

bool cause_is_indeterminate(EligibilityCause cause) noexcept {
  switch (cause) {
    case EligibilityCause::None:
    case EligibilityCause::AuthorityDenied:
    case EligibilityCause::AuthoritySuperseded:
    case EligibilityCause::HealthDegraded:
    case EligibilityCause::HealthUnavailable:
    case EligibilityCause::InsufficientHeadroom:
    case EligibilityCause::InsufficientIndependence:
    case EligibilityCause::ObligationUnprotected:
    case EligibilityCause::EvidenceTooOld:
    case EligibilityCause::ArrangementTooLarge:
    case EligibilityCause::EmptyArrangement:
    case EligibilityCause::BlockUnavailable:
      return false;

    case EligibilityCause::TopologyNotImported:
    case EligibilityCause::PolicyMissing:
    case EligibilityCause::ObligationsMissing:
    case EligibilityCause::AuthorityMissing:
    case EligibilityCause::AuthorityUnknown:
    case EligibilityCause::EpochNotEstablished:
    case EligibilityCause::EpochMismatch:
    case EligibilityCause::HealthMissing:
    case EligibilityCause::HealthUnsupported:
    case EligibilityCause::NoUsableReserve:
    case EligibilityCause::CapacityEvidenceMissing:
    case EligibilityCause::CapacityEvidenceStale:
    case EligibilityCause::BlockUnknown:
    case EligibilityCause::GroupNotInTopology:
    case EligibilityCause::GroupEmpty:
    case EligibilityCause::EvidenceFromFuture:
      return true;
  }
  return true;
}

// ---------------------------------------------------------------------------
// ReserveLedger
// ---------------------------------------------------------------------------

bool ReserveLedger::is_consumed(ReserveBlockId block) const noexcept {
  return allocations_by_block_.find(block) != allocations_by_block_.end();
}

Watts ReserveLedger::available_from(const std::vector<UsableBlock>& usable) const {
  Watts total = Watts::zero();
  for (const UsableBlock& entry : usable) {
    if (is_consumed(entry.block) || entry.available.is_negative() || entry.available.is_zero()) {
      continue;
    }
    Result<Watts> sum = Watts::add(total, entry.available);
    if (!sum.has_value()) {
      return total;
    }
    total = *sum;
  }
  return total;
}

Result<Watts> ReserveLedger::allocate(ObligationId obligation, Watts required,
                                      const std::vector<UsableBlock>& usable) {
  if (obligation.is_nil()) {
    return Status::error(ErrorCode::InvalidArgument, "obligation identity is nil");
  }
  if (required.is_negative()) {
    return Status::error(ErrorCode::OutOfRange, "required capacity is negative", "watts",
                         required.value());
  }
  for (std::size_t index = 1; index < usable.size(); ++index) {
    if (!(usable[index - 1].block < usable[index].block)) {
      return Status::error(ErrorCode::DuplicateIdentity,
                           "usable block list must be strictly ascending by block identity",
                           "index", static_cast<std::uint64_t>(index));
    }
  }

  Watts allocated = Watts::zero();
  for (const UsableBlock& entry : usable) {
    if (!(allocated < required)) {
      break;
    }
    if (is_consumed(entry.block)) {
      continue;
    }
    if (entry.available.is_negative()) {
      return Status::error(ErrorCode::OutOfRange, "usable block capacity is negative", "block",
                           entry.block.value());
    }
    if (entry.available.is_zero()) {
      continue;
    }
    Watts remaining = Watts::zero();
    {
      Result<Watts> difference = Watts::subtract(required, allocated);
      if (!difference.has_value()) {
        return difference.status();
      }
      remaining = *difference;
    }
    const Watts take = entry.available < remaining ? entry.available : remaining;
    Result<Watts> next = Watts::add(allocated, take);
    if (!next.has_value()) {
      return next.status();
    }
    allocated = *next;
    allocations_by_block_.emplace(entry.block, std::make_pair(obligation, take));
    allocations_.push_back(ReserveAllocation{entry.block, obligation, take});
  }

  Result<Watts> total = Watts::add(consumed_, allocated);
  if (!total.has_value()) {
    return total.status();
  }
  consumed_ = *total;
  return allocated;
}

Result<void> ReserveLedger::validate() const {
  if (allocations_by_block_.size() != allocations_.size()) {
    return Status::error(ErrorCode::DuplicateReserveUse,
                         "reserve ledger block map and allocation list disagree", "blocks",
                         static_cast<std::uint64_t>(allocations_by_block_.size()), "allocations",
                         static_cast<std::uint64_t>(allocations_.size()));
  }
  Watts total = Watts::zero();
  for (const ReserveAllocation& allocation : allocations_) {
    if (allocation.capacity.is_negative()) {
      return Status::error(ErrorCode::DuplicateReserveUse, "negative reserve allocation");
    }
    const auto it = allocations_by_block_.find(allocation.block);
    if (it == allocations_by_block_.end()) {
      return Status::error(ErrorCode::DuplicateReserveUse, "allocation without a block entry",
                           "block", allocation.block.value());
    }
    if (!(it->second.first == allocation.obligation) || !(it->second.second == allocation.capacity)) {
      return Status::error(ErrorCode::DuplicateReserveUse, "conflicting allocation for one block",
                           "block", allocation.block.value());
    }
    Result<Watts> sum = Watts::add(total, allocation.capacity);
    if (!sum.has_value()) {
      return sum.status();
    }
    total = *sum;
  }
  if (!(total == consumed_)) {
    return Status::error(ErrorCode::DuplicateReserveUse, "reserve ledger total does not match",
                         "sum", total.value(), "recorded", consumed_.value());
  }
  return Result<void>();
}

// ---------------------------------------------------------------------------
// Candidate identity
// ---------------------------------------------------------------------------

CandidateId derive_candidate_id(const CandidateKey& key) noexcept {
  Encoder encoder;
  encoder.u32(1);  // candidate identity scheme
  encoder.collection(key.groups);
  for (SourceGroupId group : key.groups) {
    encoder.id(group);
  }
  const Digest digest = encoder.finish_digest();
  std::uint64_t value = 0;
  const auto& bytes = digest.bytes();
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(bytes[index]) << (8U * index);
  }
  if (value == 0) {
    value = 1;  // identity 0 is reserved for "absent"
  }
  return CandidateId::from_value(value);
}

const FailoverCandidate* CandidateSet::find(const CandidateKey& key) const {
  for (const FailoverCandidate& candidate : candidates) {
    if (candidate.key == key) {
      return &candidate;
    }
  }
  return nullptr;
}

const FailoverCandidate* CandidateSet::find(CandidateId id) const {
  for (const FailoverCandidate& candidate : candidates) {
    if (candidate.id == id) {
      return &candidate;
    }
  }
  return nullptr;
}

std::size_t CandidateSet::eligible_count() const noexcept {
  std::size_t count = 0;
  for (const FailoverCandidate& candidate : candidates) {
    if (candidate.is_eligible()) {
      ++count;
    }
  }
  return count;
}

// ---------------------------------------------------------------------------
// Evidence binding
// ---------------------------------------------------------------------------

Result<Digest> compute_evidence_digest(const DomainState& state,
                                       const std::vector<SourceGroupId>& key,
                                       SourceGroupId incumbent) {
  Encoder encoder;
  encoder.u32(1);  // digest scheme version

  encoder.boolean(state.has_topology);
  if (state.has_topology) {
    encoder.id(state.topology.stamp.evidence);
    encoder.generation(state.topology.generation);
    encoder.tick(state.topology.stamp.observed_at);
  }
  encoder.boolean(state.has_capacity);
  if (state.has_capacity) {
    encoder.id(state.capacity.stamp.evidence);
    encoder.generation(state.capacity.generation);
    encoder.tick(state.capacity.stamp.observed_at);
  }
  encoder.boolean(state.has_policy);
  if (state.has_policy) {
    encoder.id(state.policy.stamp.evidence);
    encoder.generation(state.policy.generation);
    encoder.tick(state.policy.stamp.observed_at);
  }
  encoder.boolean(state.has_obligations);
  if (state.has_obligations) {
    encoder.id(state.obligations.stamp.evidence);
    encoder.u64(state.obligations.generation);
    encoder.tick(state.obligations.stamp.observed_at);
  }
  encoder.boolean(state.has_authority);
  if (state.has_authority) {
    encoder.id(state.authority.stamp.evidence);
    encoder.generation(state.authority.epoch);
    encoder.u8(static_cast<std::uint8_t>(state.authority.state));
    encoder.tick(state.authority.stamp.observed_at);
  }

  const std::vector<CoolingSourceId> sources = consumed_sources(state, key, incumbent);
  encoder.count(sources.size());
  for (CoolingSourceId source : sources) {
    encoder.id(source);
    const auto it = state.health.find(source);
    encoder.boolean(it != state.health.end());
    if (it != state.health.end()) {
      encoder.id(it->second.stamp.evidence);
      encoder.u8(static_cast<std::uint8_t>(it->second.state));
      encoder.tick(it->second.stamp.observed_at);
    }
  }
  if (!encoder.ok()) {
    return encoder.status();
  }
  return encoder.finish_digest();
}

// ---------------------------------------------------------------------------
// Arrangement evaluation
// ---------------------------------------------------------------------------

Result<FailoverCandidate> evaluate_arrangement(const DomainState& state, const CandidateKey& key,
                                               Tick now) {
  for (std::size_t index = 1; index < key.groups.size(); ++index) {
    if (!(key.groups[index - 1] < key.groups[index])) {
      return Status::error(ErrorCode::DuplicateIdentity,
                           "candidate arrangement must be strictly ascending by group identity",
                           "index", static_cast<std::uint64_t>(index));
    }
  }

  FailoverCandidate candidate;
  candidate.key = key;
  candidate.domain = state.domain;
  candidate.epoch = state.epoch;
  candidate.topology = state.topology.generation;
  candidate.capacity = state.capacity.generation;
  candidate.policy = state.policy.generation;
  candidate.evidence_generation = state.evidence_generation;
  candidate.id = derive_candidate_id(key);
  candidate.independent_groups = static_cast<std::uint32_t>(key.groups.size());

  std::vector<EligibilityCause> causes;
  const auto add = [&causes](EligibilityCause cause) { add_cause(causes, cause); };

  AgeCheck age;
  age.now = now;
  if (state.has_policy) {
    age.enabled = state.policy.max_evidence_age_ticks > 0;
    age.max_age = state.policy.max_evidence_age_ticks;
  }

  // 1. Arrangement shape.
  if (key.groups.empty()) {
    add(EligibilityCause::EmptyArrangement);
  } else if (key.groups.size() > kMaxGroupsPerCandidate) {
    add(EligibilityCause::ArrangementTooLarge);
  }

  // 2. Topology binding.
  if (!state.has_topology) {
    add(EligibilityCause::TopologyNotImported);
  } else {
    if (age.from_future(state.topology.stamp.observed_at)) {
      add(EligibilityCause::EvidenceFromFuture);
    }
    if (age.stale(state.topology.stamp.observed_at)) {
      add(EligibilityCause::EvidenceTooOld);
    }
    for (SourceGroupId group : key.groups) {
      if (!state.topology.contains_group(group)) {
        add(EligibilityCause::GroupNotInTopology);
      } else if (state.topology.members_of(group).empty()) {
        add(EligibilityCause::GroupEmpty);
      }
    }
  }

  // 3. Policy and obligations.
  if (!state.has_policy) {
    add(EligibilityCause::PolicyMissing);
  } else {
    if (age.from_future(state.policy.stamp.observed_at)) {
      add(EligibilityCause::EvidenceFromFuture);
    }
    if (age.stale(state.policy.stamp.observed_at)) {
      add(EligibilityCause::EvidenceTooOld);
    }
  }
  if (!state.has_obligations) {
    add(EligibilityCause::ObligationsMissing);
  }

  // 4. Authority and epoch.
  if (!state.epoch_established) {
    add(EligibilityCause::EpochNotEstablished);
  }
  if (!state.has_authority) {
    add(EligibilityCause::AuthorityMissing);
  } else {
    if (!(state.authority.epoch == state.epoch)) {
      add(EligibilityCause::EpochMismatch);
    }
    switch (state.authority.state) {
      case AuthorityState::Granted:
        break;
      case AuthorityState::Denied:
        add(EligibilityCause::AuthorityDenied);
        break;
      case AuthorityState::Superseded:
        add(EligibilityCause::AuthoritySuperseded);
        break;
      case AuthorityState::Unknown:
        add(EligibilityCause::AuthorityUnknown);
        break;
    }
    if (age.from_future(state.authority.stamp.observed_at)) {
      add(EligibilityCause::EvidenceFromFuture);
    }
    if (age.stale(state.authority.stamp.observed_at)) {
      add(EligibilityCause::EvidenceTooOld);
    }
  }

  // 5. Capacity evidence presence.
  if (!state.has_capacity) {
    add(EligibilityCause::CapacityEvidenceMissing);
  } else {
    if (age.from_future(state.capacity.stamp.observed_at)) {
      add(EligibilityCause::EvidenceFromFuture);
    }
    if (age.stale(state.capacity.stamp.observed_at)) {
      add(EligibilityCause::CapacityEvidenceStale);
    }
  }

  // 6. Independence.
  if (state.has_policy &&
      candidate.independent_groups < state.policy.minimum_independent_groups) {
    add(EligibilityCause::InsufficientIndependence);
  }
  if (state.has_obligations) {
    for (const ProtectedObligation& obligation : state.obligations.obligations) {
      if (candidate.independent_groups < obligation.required_independent_groups) {
        add(EligibilityCause::ObligationUnprotected);
        break;
      }
    }
  }

  // 7. Health of every member source of the arrangement.
  bool health_unknown = false;
  if (state.has_topology) {
    for (SourceGroupId group : key.groups) {
      if (!state.topology.contains_group(group)) {
        continue;
      }
      std::uint32_t healthy_members = 0;
      std::uint32_t known_members = 0;
      for (CoolingSourceId member : state.topology.members_of(group)) {
        const auto it = state.health.find(member);
        if (it == state.health.end()) {
          continue;
        }
        if (age.from_future(it->second.stamp.observed_at)) {
          add(EligibilityCause::EvidenceFromFuture);
          continue;
        }
        switch (it->second.state) {
          case HealthState::Healthy:
            ++healthy_members;
            ++known_members;
            break;
          case HealthState::Degraded:
            ++known_members;
            add(EligibilityCause::HealthDegraded);
            break;
          case HealthState::Unavailable:
            ++known_members;
            add(EligibilityCause::HealthUnavailable);
            break;
          case HealthState::Unsupported:
            add(EligibilityCause::HealthUnsupported);
            health_unknown = true;
            break;
          case HealthState::Unknown:
            add(EligibilityCause::HealthMissing);
            health_unknown = true;
            break;
        }
      }
      if (known_members == 0) {
        add(EligibilityCause::HealthMissing);
        health_unknown = true;
      }
      (void)healthy_members;
    }
  }

  // 8. Usable reserve blocks. A block that several groups in the arrangement
  //    can draw on is counted exactly once.
  Watts usable_total = Watts::zero();
  std::vector<UsableBlock> usable;
  bool reserve_unknown = false;
  if (state.has_topology && state.has_capacity) {
    std::vector<ReserveBlockId> blocks;
    for (SourceGroupId group : key.groups) {
      for (ReserveBlockId block : state.topology.blocks_serving(group)) {
        blocks.push_back(block);
      }
    }
    std::sort(blocks.begin(), blocks.end());
    blocks.erase(std::unique(blocks.begin(), blocks.end()), blocks.end());

    for (ReserveBlockId block : blocks) {
      const ReserveBlockDescriptor* descriptor = find_block(state.topology, block);
      if (descriptor == nullptr) {
        add(EligibilityCause::BlockUnknown);
        reserve_unknown = true;
        continue;
      }
      const auto health_it = state.health.find(descriptor->owner_source);
      if (health_it == state.health.end()) {
        add(EligibilityCause::HealthMissing);
        reserve_unknown = true;
        continue;
      }
      switch (health_it->second.state) {
        case HealthState::Healthy:
          break;
        case HealthState::Degraded:
          add(EligibilityCause::HealthDegraded);
          continue;
        case HealthState::Unavailable:
          add(EligibilityCause::HealthUnavailable);
          continue;
        case HealthState::Unsupported:
          add(EligibilityCause::HealthUnsupported);
          reserve_unknown = true;
          continue;
        case HealthState::Unknown:
          add(EligibilityCause::HealthMissing);
          reserve_unknown = true;
          continue;
      }
      if (age.stale(health_it->second.stamp.observed_at)) {
        add(EligibilityCause::EvidenceTooOld);
        continue;
      }
      const BlockAvailability* availability = state.capacity.find(block);
      if (availability == nullptr) {
        add(EligibilityCause::BlockUnknown);
        reserve_unknown = true;
        continue;
      }
      switch (availability->state) {
        case AvailabilityState::Available:
          if (availability->available.value() <= 0) {
            add(EligibilityCause::BlockUnknown);
            reserve_unknown = true;
            break;
          }
          usable.push_back(UsableBlock{block, availability->available,
                                       group_in(key.groups, descriptor->owner_group)});
          {
            Result<Watts> sum = Watts::add(usable_total, availability->available);
            if (!sum.has_value()) {
              return sum.status();
            }
            usable_total = *sum;
          }
          break;
        case AvailabilityState::Zero:
        case AvailabilityState::Unavailable:
          add(EligibilityCause::BlockUnavailable);
          break;
        case AvailabilityState::Unknown:
        case AvailabilityState::Unsupported:
        case AvailabilityState::Indeterminate:
          add(EligibilityCause::BlockUnknown);
          reserve_unknown = true;
          break;
      }
    }
    if (usable.empty() && !blocks.empty()) {
      add(EligibilityCause::NoUsableReserve);
    }
  }

  // 9. Headroom against the protected obligations.
  Watts total_required = Watts::zero();
  if (state.has_obligations) {
    std::vector<std::int64_t> required;
    required.reserve(state.obligations.obligations.size());
    for (const ProtectedObligation& obligation : state.obligations.obligations) {
      if (obligation.required_capacity.is_negative()) {
        return Status::error(ErrorCode::OutOfRange, "obligation requires negative capacity",
                             "obligation", obligation.id.value());
      }
      required.push_back(obligation.required_capacity.value());
    }
    Result<Watts> sum = Watts::sum(required.data(), required.size());
    if (!sum.has_value()) {
      return sum.status();
    }
    total_required = *sum;
  }

  Watts required_with_margin = total_required;
  if (state.has_policy) {
    Result<Watts> margin = Watts::scale_up(total_required, state.policy.headroom_margin);
    if (!margin.has_value()) {
      return margin.status();
    }
    Result<Watts> with_margin = Watts::add(total_required, *margin);
    if (!with_margin.has_value()) {
      return with_margin.status();
    }
    required_with_margin = *with_margin;
  }

  const bool unknown_evidence = health_unknown || reserve_unknown || any_indeterminate(causes);
  if (!(usable_total >= required_with_margin)) {
    if (!unknown_evidence) {
      add(EligibilityCause::InsufficientHeadroom);
    }
  }

  // 10. Reserve allocation. The ledger refuses to allocate a block twice, so a
  //     reserve cannot be counted twice inside one arrangement.
  if (state.has_obligations) {
    ReserveLedger ledger;
    for (const ProtectedObligation& obligation : state.obligations.obligations) {
      Result<Watts> allocated = ledger.allocate(obligation.id, obligation.required_capacity, usable);
      if (!allocated.has_value()) {
        return allocated.status();
      }
      if (*allocated < obligation.required_capacity && !unknown_evidence) {
        add(EligibilityCause::ObligationUnprotected);
      }
    }
    Result<void> ledger_ok = ledger.validate();
    if (!ledger_ok.has_value()) {
      return ledger_ok.status();
    }
    candidate.allocations = ledger.allocations();
  }

  Result<Watts> surplus = Watts::subtract(usable_total, total_required);
  if (!surplus.has_value()) {
    return surplus.status();
  }
  candidate.surplus = *surplus;
  candidate.usable_capacity = usable_total;
  candidate.causes = causes;
  if (any_ineligible(causes)) {
    candidate.state = EligibilityState::Ineligible;
  } else if (any_indeterminate(causes)) {
    candidate.state = EligibilityState::Indeterminate;
  } else {
    candidate.state = EligibilityState::Eligible;
  }
  return candidate;
}

// ---------------------------------------------------------------------------
// Ranking
// ---------------------------------------------------------------------------

Result<void> rank_candidates(std::vector<FailoverCandidate>& candidates,
                             const RedundancyPolicy& policy) {
  if (policy.rank_order.size() > kMaxRankCriteria) {
    return Status::error(ErrorCode::BoundsExceeded, "rank order has too many criteria", "count",
                         static_cast<std::uint64_t>(policy.rank_order.size()));
  }
  std::vector<std::size_t> eligible;
  eligible.reserve(candidates.size());
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    candidates[index].rank = UINT32_MAX;
    if (candidates[index].is_eligible()) {
      eligible.push_back(index);
    }
  }

  const std::vector<RankCriterion>& order = policy.rank_order;
  std::stable_sort(eligible.begin(), eligible.end(), [&](std::size_t lhs, std::size_t rhs) {
    const FailoverCandidate& left = candidates[lhs];
    const FailoverCandidate& right = candidates[rhs];
    for (RankCriterion criterion : order) {
      switch (criterion) {
        case RankCriterion::AscendingGroupKey:
          if (left.key < right.key) {
            return true;
          }
          if (right.key < left.key) {
            return false;
          }
          break;
        case RankCriterion::DescendingUsableCapacity:
          if (right.usable_capacity < left.usable_capacity) {
            return true;
          }
          if (left.usable_capacity < right.usable_capacity) {
            return false;
          }
          break;
        case RankCriterion::DescendingHeadroom:
          if (right.surplus < left.surplus) {
            return true;
          }
          if (left.surplus < right.surplus) {
            return false;
          }
          break;
        case RankCriterion::FewestGroups:
          if (left.independent_groups != right.independent_groups) {
            return left.independent_groups < right.independent_groups;
          }
          break;
        case RankCriterion::MostGroups:
          if (left.independent_groups != right.independent_groups) {
            return left.independent_groups > right.independent_groups;
          }
          break;
        case RankCriterion::OwnerGroupFirst: {
          const std::uint32_t left_owned =
              static_cast<std::uint32_t>(std::count_if(left.allocations.begin(),
                                                       left.allocations.end(),
                                                       [](const ReserveAllocation&) { return true; }));
          const std::uint32_t right_owned =
              static_cast<std::uint32_t>(std::count_if(right.allocations.begin(),
                                                       right.allocations.end(),
                                                       [](const ReserveAllocation&) { return true; }));
          if (left_owned != right_owned) {
            return left_owned > right_owned;
          }
          break;
        }
      }
    }
    // Final, total tie-break so the ordering is always strict and deterministic.
    return left.key < right.key;
  });

  for (std::size_t rank = 0; rank < eligible.size(); ++rank) {
    candidates[eligible[rank]].rank = static_cast<std::uint32_t>(rank);
  }
  return Result<void>();
}

// ---------------------------------------------------------------------------
// Enumeration
// ---------------------------------------------------------------------------

Result<std::vector<CandidateKey>> enumerate_arrangements(const DomainState& state) {
  if (!state.has_topology) {
    return Status::error(ErrorCode::TopologyNotImported, "no topology projection has been imported");
  }
  if (!state.has_policy) {
    return Status::error(ErrorCode::PolicyNotDefined, "no redundancy policy is installed");
  }
  const std::vector<SourceGroupDescriptor>& groups = state.topology.groups;
  if (groups.size() > kMaxGroupsPerDomain) {
    return Status::error(ErrorCode::BoundsExceeded, "topology has too many source groups", "groups",
                         static_cast<std::uint64_t>(groups.size()));
  }
  std::uint32_t max_size = state.policy.max_groups_per_candidate;
  if (max_size == 0) {
    max_size = 1;
  }
  if (max_size > kMaxGroupsPerCandidate) {
    max_size = kMaxGroupsPerCandidate;
  }
  if (max_size > groups.size()) {
    max_size = static_cast<std::uint32_t>(groups.size());
  }

  std::vector<CandidateKey> arrangements;
  const std::size_t count = groups.size();
  const std::uint64_t subsets = (count >= 63) ? 0 : (1ULL << count);
  for (std::uint64_t mask = 1; mask < subsets; ++mask) {
    std::uint32_t size = 0;
    for (std::size_t bit = 0; bit < count; ++bit) {
      if ((mask & (1ULL << bit)) != 0) {
        ++size;
      }
    }
    if (size == 0 || size > max_size) {
      continue;
    }
    CandidateKey key;
    key.groups.reserve(size);
    for (std::size_t bit = 0; bit < count; ++bit) {
      if ((mask & (1ULL << bit)) != 0) {
        key.groups.push_back(groups[bit].group);
      }
    }
    if (arrangements.size() >= kMaxCandidateSubsets) {
      return Status::error(ErrorCode::LimitExceeded, "candidate enumeration exceeded its bound",
                           "bound", static_cast<std::uint64_t>(kMaxCandidateSubsets));
    }
    arrangements.push_back(std::move(key));
  }

  std::sort(arrangements.begin(), arrangements.end(),
            [](const CandidateKey& lhs, const CandidateKey& rhs) {
              if (lhs.groups.size() != rhs.groups.size()) {
                return lhs.groups.size() < rhs.groups.size();
              }
              return lhs < rhs;
            });
  return arrangements;
}

}  // namespace cooling_failover
