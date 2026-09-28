#include "cooling_failover/state.hpp"

#include <algorithm>
#include <set>

namespace cooling_failover {
namespace {

void encode_stamp(Encoder& encoder, const EvidenceStamp& stamp) {
  encoder.id(stamp.evidence);
  encoder.u8(static_cast<std::uint8_t>(stamp.kind));
  encoder.u8(static_cast<std::uint8_t>(stamp.owner));
  encoder.tick(stamp.observed_at);
  encoder.text(stamp.reference);
}

void recompute_evidence_digest(DomainState& state) {
  Encoder encoder;
  encoder.u32(1);
  encoder.boolean(state.has_topology);
  if (state.has_topology) {
    encode_stamp(encoder, state.topology.stamp);
    encoder.generation(state.topology.generation);
  }
  encoder.boolean(state.has_capacity);
  if (state.has_capacity) {
    encode_stamp(encoder, state.capacity.stamp);
    encoder.generation(state.capacity.generation);
  }
  encoder.boolean(state.has_policy);
  if (state.has_policy) {
    encode_stamp(encoder, state.policy.stamp);
    encoder.generation(state.policy.generation);
  }
  encoder.boolean(state.has_obligations);
  if (state.has_obligations) {
    encode_stamp(encoder, state.obligations.stamp);
    encoder.u64(state.obligations.generation);
  }
  encoder.boolean(state.has_authority);
  if (state.has_authority) {
    encode_stamp(encoder, state.authority.stamp);
    encoder.generation(state.authority.epoch);
    encoder.u8(static_cast<std::uint8_t>(state.authority.state));
  }
  encoder.count(state.health.size());
  for (const auto& entry : state.health) {
    encoder.id(entry.first);
    encode_stamp(encoder, entry.second.stamp);
    encoder.u8(static_cast<std::uint8_t>(entry.second.state));
  }
  state.evidence_digest = encoder.finish_digest();
}

}  // namespace

void DomainState::touch_evidence(Tick now) {
  last_tick = now;
  if (evidence_generation.value() < UINT64_MAX) {
    evidence_generation = EvidenceSetGeneration::from_value(evidence_generation.value() + 1);
  }
  recompute_evidence_digest(*this);
}

Result<void> DomainState::validate() const {
  if (domain.is_nil()) {
    return Status::error(ErrorCode::InvalidArgument, "domain identity is nil");
  }
  if (has_topology) {
    if (topology.groups.size() > kMaxGroupsPerDomain) {
      return Status::error(ErrorCode::BoundsExceeded, "topology has too many groups");
    }
    if (topology.sources.size() > kMaxSourcesPerDomain) {
      return Status::error(ErrorCode::BoundsExceeded, "topology has too many sources");
    }
    if (topology.blocks.size() > kMaxBlocksPerDomain) {
      return Status::error(ErrorCode::BoundsExceeded, "topology has too many reserve blocks");
    }
    for (std::size_t index = 1; index < topology.groups.size(); ++index) {
      if (!(topology.groups[index - 1].group < topology.groups[index].group)) {
        return Status::error(ErrorCode::DuplicateIdentity, "topology groups are not strictly ordered");
      }
    }
    for (std::size_t index = 1; index < topology.sources.size(); ++index) {
      if (!(topology.sources[index - 1].source < topology.sources[index].source)) {
        return Status::error(ErrorCode::DuplicateIdentity,
                             "topology sources are not strictly ordered");
      }
    }
    for (std::size_t index = 1; index < topology.blocks.size(); ++index) {
      if (!(topology.blocks[index - 1].block < topology.blocks[index].block)) {
        return Status::error(ErrorCode::DuplicateIdentity,
                             "topology reserve blocks are not strictly ordered");
      }
    }
  }
  if (has_obligations) {
    if (obligations.obligations.size() > kMaxObligationsPerDomain) {
      return Status::error(ErrorCode::BoundsExceeded, "too many protected obligations");
    }
    for (std::size_t index = 1; index < obligations.obligations.size(); ++index) {
      if (!(obligations.obligations[index - 1].id < obligations.obligations[index].id)) {
        return Status::error(ErrorCode::DuplicateIdentity, "obligations are not strictly ordered");
      }
    }
  }
  if (has_policy) {
    if (policy.rank_order.size() > kMaxRankCriteria) {
      return Status::error(ErrorCode::BoundsExceeded, "policy has too many rank criteria");
    }
    if (policy.max_groups_per_candidate > kMaxGroupsPerCandidate) {
      return Status::error(ErrorCode::BoundsExceeded, "policy allows too many groups per candidate");
    }
    if (policy.max_plan_steps > kMaxPlanSteps) {
      return Status::error(ErrorCode::BoundsExceeded, "policy allows too many plan steps");
    }
  }
  if (plans.size() > kMaxRetainedPlans) {
    return Status::error(ErrorCode::BoundsExceeded, "too many retained plans");
  }
  if (attempts.size() > kMaxRetainedAttempts) {
    return Status::error(ErrorCode::BoundsExceeded, "too many retained attempts");
  }
  if (commands.size() > kMaxRetainedCommands) {
    return Status::error(ErrorCode::BoundsExceeded, "too many retained commands");
  }
  if (observations.size() > kMaxRetainedObservations) {
    return Status::error(ErrorCode::BoundsExceeded, "too many retained observations");
  }
  if (transitions.size() > kMaxTransitionHistory) {
    return Status::error(ErrorCode::BoundsExceeded, "transition history exceeds its bound");
  }
  if (!active_plan.is_nil()) {
    const auto it = plans.find(active_plan);
    if (it == plans.end()) {
      return Status::error(ErrorCode::PlanNotFound, "the active plan slot references an unknown plan");
    }
    if (it->second.state != PlanState::Active) {
      return Status::error(ErrorCode::PlanNotActive, "the active plan slot references a closed plan");
    }
  }
  return Result<void>();
}

// ---------------------------------------------------------------------------
// Event payload encoders
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> encode_domain_registered(Tick now) {
  Encoder encoder;
  encoder.tick(now);
  return encoder.take();
}

std::vector<std::uint8_t> encode_epoch_established(ControlPlaneEpoch epoch, Tick now) {
  Encoder encoder;
  encoder.generation(epoch);
  encoder.tick(now);
  return encoder.take();
}

std::vector<std::uint8_t> encode_topology(const TopologyProjection& topology) {
  Encoder encoder;
  encode_stamp(encoder, topology.stamp);
  encoder.id(topology.domain);
  encoder.generation(topology.generation);
  encoder.collection(topology.groups);
  for (const SourceGroupDescriptor& group : topology.groups) {
    encoder.id(group.group);
    encoder.watts(group.nominal_capacity);
    encoder.boolean(group.designated_primary);
  }
  encoder.collection(topology.sources);
  for (const SourceDescriptor& source : topology.sources) {
    encoder.id(source.source);
    encoder.id(source.group);
    encoder.u8(static_cast<std::uint8_t>(source.kind));
    encoder.watts(source.nominal_capacity);
  }
  encoder.collection(topology.blocks);
  for (const ReserveBlockDescriptor& block : topology.blocks) {
    encoder.id(block.block);
    encoder.id(block.owner_group);
    encoder.id(block.owner_source);
    encoder.watts(block.nominal_capacity);
    encoder.collection(block.serving_groups);
    for (SourceGroupId group : block.serving_groups) {
      encoder.id(group);
    }
  }
  return encoder.take();
}

std::vector<std::uint8_t> encode_capacity(const CapacityAvailabilityEvidence& evidence) {
  Encoder encoder;
  encode_stamp(encoder, evidence.stamp);
  encoder.id(evidence.domain);
  encoder.generation(evidence.generation);
  encoder.collection(evidence.blocks);
  for (const BlockAvailability& block : evidence.blocks) {
    encoder.id(block.block);
    encoder.u8(static_cast<std::uint8_t>(block.state));
    encoder.watts(block.available);
    encoder.tick(block.observed_at);
  }
  return encoder.take();
}

std::vector<std::uint8_t> encode_health(const SourceHealthEvidence& evidence) {
  Encoder encoder;
  encode_stamp(encoder, evidence.stamp);
  encoder.id(evidence.source);
  encoder.u8(static_cast<std::uint8_t>(evidence.state));
  return encoder.take();
}

std::vector<std::uint8_t> encode_authority(const ServiceAuthorityEvidence& evidence) {
  Encoder encoder;
  encode_stamp(encoder, evidence.stamp);
  encoder.id(evidence.domain);
  encoder.generation(evidence.epoch);
  encoder.u8(static_cast<std::uint8_t>(evidence.state));
  return encoder.take();
}

std::vector<std::uint8_t> encode_policy(const RedundancyPolicy& policy) {
  Encoder encoder;
  encode_stamp(encoder, policy.stamp);
  encoder.generation(policy.generation);
  encoder.u8(static_cast<std::uint8_t>(policy.selection));
  encoder.collection(policy.rank_order);
  for (RankCriterion criterion : policy.rank_order) {
    encoder.u8(static_cast<std::uint8_t>(criterion));
  }
  encoder.u32(policy.minimum_independent_groups);
  encoder.u32(policy.max_groups_per_candidate);
  encoder.basis_points(policy.headroom_margin);
  encoder.u32(policy.recovery_consecutive_healthy);
  encoder.u64(policy.recovery_dwell_ticks);
  encoder.u64(policy.max_evidence_age_ticks);
  encoder.u64(policy.oscillation_window_ticks);
  encoder.u32(policy.max_transitions_per_window);
  encoder.u32(policy.max_plan_steps);
  return encoder.take();
}

std::vector<std::uint8_t> encode_obligations(const ObligationSet& obligations) {
  Encoder encoder;
  encode_stamp(encoder, obligations.stamp);
  encoder.u64(obligations.generation);
  encoder.collection(obligations.obligations);
  for (const ProtectedObligation& obligation : obligations.obligations) {
    encoder.id(obligation.id);
    encoder.watts(obligation.required_capacity);
    encoder.u32(obligation.required_independent_groups);
    encoder.u8(static_cast<std::uint8_t>(obligation.criticality));
    encoder.text(obligation.reference);
  }
  return encoder.take();
}

std::vector<std::uint8_t> encode_plan(const FailoverPlan& plan) {
  Encoder encoder;
  encoder.id(plan.id);
  encoder.id(plan.request);
  encoder.digest(plan.request_fingerprint);
  encoder.id(plan.domain);
  encoder.u8(static_cast<std::uint8_t>(plan.kind));
  encoder.generation(plan.epoch);
  encoder.counter(plan.revision_at_creation);
  encoder.generation(plan.topology);
  encoder.generation(plan.capacity);
  encoder.generation(plan.policy);
  encoder.generation(plan.evidence_generation);
  encoder.digest(plan.evidence_digest);
  encoder.u64(plan.obligations_generation);
  encoder.id(plan.incumbent);
  encoder.id(plan.target);
  encoder.collection(plan.target_groups);
  for (SourceGroupId group : plan.target_groups) {
    encoder.id(group);
  }
  encoder.collection(plan.steps);
  for (const PlanStep& step : plan.steps) {
    encoder.u32(step.ordinal);
    encoder.id(step.target);
    encoder.collection(step.required_effects);
    for (EffectKind effect : step.required_effects) {
      encoder.u8(static_cast<std::uint8_t>(effect));
    }
  }
  encoder.collection(plan.commands);
  for (const CommandSpec& command : plan.commands) {
    encoder.id(command.id);
    encoder.id(command.key);
    encoder.digest(command.fingerprint);
    encoder.u32(command.step);
    encoder.u8(static_cast<std::uint8_t>(command.runtime));
    encoder.id(command.target);
    encoder.collection(command.required_effects);
    for (EffectKind effect : command.required_effects) {
      encoder.u8(static_cast<std::uint8_t>(effect));
    }
  }
  encoder.tick(plan.created_at);
  encoder.u8(static_cast<std::uint8_t>(plan.state));
  encoder.u8(static_cast<std::uint8_t>(plan.fence_reason));
  encoder.tick(plan.fenced_at);
  encoder.u32(plan.attempts_started);
  encoder.text(plan.reason);
  return encoder.take();
}

std::vector<std::uint8_t> encode_plan_state(PlanId plan, PlanState state, FenceReason reason,
                                            Tick at) {
  Encoder encoder;
  encoder.id(plan);
  encoder.u8(static_cast<std::uint8_t>(state));
  encoder.u8(static_cast<std::uint8_t>(reason));
  encoder.tick(at);
  return encoder.take();
}

std::vector<std::uint8_t> encode_command(const CommandRecord& command) {
  Encoder encoder;
  encoder.id(command.id);
  encoder.id(command.key);
  encoder.digest(command.fingerprint);
  encoder.id(command.plan);
  encoder.id(command.attempt);
  encoder.u32(command.step);
  encoder.u8(static_cast<std::uint8_t>(command.runtime));
  encoder.id(command.target);
  encoder.u8(static_cast<std::uint8_t>(command.outcome));
  encoder.counter(command.issue_sequence);
  encoder.tick(command.issued_at);
  encoder.generation(command.epoch);
  encoder.text(command.detail);
  return encoder.take();
}

std::vector<std::uint8_t> encode_observation(const EffectObservationRecord& observation) {
  Encoder encoder;
  encoder.id(observation.id);
  encoder.id(observation.command);
  encoder.id(observation.group);
  encoder.u8(static_cast<std::uint8_t>(observation.effect));
  encoder.u8(static_cast<std::uint8_t>(observation.origin));
  encoder.counter(observation.sequence);
  encoder.generation(observation.topology);
  encoder.generation(observation.capacity);
  encoder.generation(observation.epoch);
  encoder.tick(observation.observed_at);
  encoder.boolean(observation.recovered_unvalidated);
  return encoder.take();
}

std::vector<std::uint8_t> encode_attempt(const TransitionAttempt& attempt) {
  Encoder encoder;
  encoder.id(attempt.id);
  encoder.id(attempt.plan);
  encoder.u32(attempt.number);
  encoder.generation(attempt.epoch);
  encoder.u8(static_cast<std::uint8_t>(attempt.state));
  encoder.tick(attempt.created_at);
  encoder.tick(attempt.updated_at);
  encoder.counter(attempt.revision_at_issue);
  encoder.digest(attempt.plan_binding);
  encoder.collection(attempt.commands);
  for (CommandId command : attempt.commands) {
    encoder.id(command);
  }
  encoder.collection(attempt.verified_effects);
  for (EffectKind effect : attempt.verified_effects) {
    encoder.u8(static_cast<std::uint8_t>(effect));
  }
  encoder.collection(attempt.missing_effects);
  for (EffectKind effect : attempt.missing_effects) {
    encoder.u8(static_cast<std::uint8_t>(effect));
  }
  encoder.u32(attempt.commands_issued);
  encoder.u32(attempt.commands_replayed);
  encoder.u32(attempt.commands_refused);
  encoder.u32(attempt.observations_accepted);
  encoder.text(attempt.detail);
  return encoder.take();
}

std::vector<std::uint8_t> encode_health_streak(const HealthStreak& streak) {
  Encoder encoder;
  encoder.id(streak.group);
  encoder.u32(streak.consecutive_healthy);
  encoder.tick(streak.streak_started_at);
  encoder.tick(streak.last_observed_at);
  encoder.generation(streak.topology);
  encoder.generation(streak.capacity);
  return encoder.take();
}

std::vector<std::uint8_t> encode_transition(const TransitionRecord& record) {
  Encoder encoder;
  encoder.id(record.plan);
  encoder.u8(static_cast<std::uint8_t>(record.kind));
  encoder.id(record.from_group);
  encoder.id(record.to_group);
  encoder.tick(record.at);
  return encoder.take();
}

}  // namespace cooling_failover
