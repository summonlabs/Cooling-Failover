#include "cooling_failover/state.hpp"

#include <algorithm>
#include <set>

namespace cooling_failover {
namespace {

[[nodiscard]] Result<EvidenceStamp> read_stamp(Decoder& decoder) {
  EvidenceStamp stamp;
  stamp.evidence = decoder.id<EvidenceTag>();
  const std::uint8_t kind = decoder.u8();
  const std::uint8_t owner = decoder.u8();
  stamp.observed_at = decoder.tick();
  stamp.reference = decoder.text();
  if (!decoder.ok()) {
    return decoder.status();
  }
  if (kind < static_cast<std::uint8_t>(EvidenceKind::TopologyProjection) ||
      kind > static_cast<std::uint8_t>(EvidenceKind::EffectObservation)) {
    return Status::error(ErrorCode::InvalidEnumValue, "evidence kind out of range");
  }
  if (owner < static_cast<std::uint8_t>(OwnerSystem::CoolingTopology) ||
      owner > static_cast<std::uint8_t>(OwnerSystem::ActuationControllerAck)) {
    return Status::error(ErrorCode::InvalidEnumValue, "evidence owner out of range");
  }
  stamp.kind = static_cast<EvidenceKind>(kind);
  stamp.owner = static_cast<OwnerSystem>(owner);
  return stamp;
}

/// Reads an enumeration whose encoded range is [p min_value, p max_value].
template <class Enum>
[[nodiscard]] Result<Enum> read_enum_range(Decoder& decoder, std::uint8_t min_value,
                                           std::uint8_t max_value, const char* what) {
  const std::uint8_t raw = decoder.u8();
  if (!decoder.ok()) {
    return decoder.status();
  }
  if (raw < min_value || raw > max_value) {
    return Status::error(ErrorCode::InvalidEnumValue, what);
  }
  return static_cast<Enum>(raw);
}

/// Reads an enumeration whose encoded range starts at 1 (0 is reserved).
template <class Enum>
[[nodiscard]] Result<Enum> read_enum(Decoder& decoder, std::uint8_t max_value, const char* what) {
  return read_enum_range<Enum>(decoder, 1, max_value, what);
}

[[nodiscard]] Result<TopologyProjection> read_topology(Decoder& decoder,
                                                       FailoverDomainId domain) {
  TopologyProjection topology;
  CF_TRY_ASSIGN(stamp, read_stamp(decoder));
  topology.stamp = stamp;
  topology.domain = decoder.id<FailoverDomainTag>();
  topology.generation = decoder.generation<TopologyGenerationTag>();
  if (!decoder.ok()) {
    return decoder.status();
  }
  if (!(topology.domain == domain)) {
    return Status::error(ErrorCode::StoreCorrupt, "topology event belongs to another domain");
  }

  const std::size_t group_count = decoder.count(kMaxGroupsPerDomain);
  if (!decoder.ok()) {
    return decoder.status();
  }
  topology.groups.reserve(group_count);
  for (std::size_t index = 0; index < group_count; ++index) {
    SourceGroupDescriptor group;
    group.group = decoder.id<SourceGroupTag>();
    group.nominal_capacity = decoder.watts();
    group.designated_primary = decoder.boolean();
    if (!decoder.ok()) {
      return decoder.status();
    }
    if (group.group.is_nil()) {
      return Status::error(ErrorCode::InvalidArgument, "topology group identity is nil");
    }
    if (index > 0 && !(topology.groups[index - 1].group < group.group)) {
      return Status::error(ErrorCode::DuplicateIdentity, "topology groups are not strictly ordered");
    }
    topology.groups.push_back(group);
  }

  const std::size_t source_count = decoder.count(kMaxSourcesPerDomain);
  if (!decoder.ok()) {
    return decoder.status();
  }
  topology.sources.reserve(source_count);
  for (std::size_t index = 0; index < source_count; ++index) {
    SourceDescriptor source;
    source.source = decoder.id<CoolingSourceTag>();
    source.group = decoder.id<SourceGroupTag>();
    CF_TRY_ASSIGN(kind, read_enum<SourceKind>(decoder, static_cast<std::uint8_t>(SourceKind::ThermalStore),
                                              "source kind out of range"));
    source.kind = kind;
    source.nominal_capacity = decoder.watts();
    if (!decoder.ok()) {
      return decoder.status();
    }
    if (source.source.is_nil() || source.group.is_nil()) {
      return Status::error(ErrorCode::InvalidArgument, "topology source identity is nil");
    }
    if (index > 0 && !(topology.sources[index - 1].source < source.source)) {
      return Status::error(ErrorCode::DuplicateIdentity, "topology sources are not strictly ordered");
    }
    if (!topology.contains_group(source.group)) {
      return Status::error(ErrorCode::InvalidArgument,
                           "topology source references an unknown group", "source",
                           source.source.value());
    }
    topology.sources.push_back(source);
  }

  const std::size_t block_count = decoder.count(kMaxBlocksPerDomain);
  if (!decoder.ok()) {
    return decoder.status();
  }
  topology.blocks.reserve(block_count);
  for (std::size_t index = 0; index < block_count; ++index) {
    ReserveBlockDescriptor block;
    block.block = decoder.id<ReserveBlockTag>();
    block.owner_group = decoder.id<SourceGroupTag>();
    block.owner_source = decoder.id<CoolingSourceTag>();
    block.nominal_capacity = decoder.watts();
    const std::size_t serving = decoder.count(kMaxGroupsPerDomain);
    if (!decoder.ok()) {
      return decoder.status();
    }
    for (std::size_t slot = 0; slot < serving; ++slot) {
      block.serving_groups.push_back(decoder.id<SourceGroupTag>());
    }
    if (!decoder.ok()) {
      return decoder.status();
    }
    if (block.block.is_nil()) {
      return Status::error(ErrorCode::InvalidArgument, "topology reserve block identity is nil");
    }
    if (index > 0 && !(topology.blocks[index - 1].block < block.block)) {
      return Status::error(ErrorCode::DuplicateIdentity,
                           "topology reserve blocks are not strictly ordered");
    }
    if (serving == 0) {
      return Status::error(ErrorCode::InvalidArgument,
                           "topology reserve block serves no source group");
    }
    for (std::size_t slot = 1; slot < block.serving_groups.size(); ++slot) {
      if (!(block.serving_groups[slot - 1] < block.serving_groups[slot])) {
        return Status::error(ErrorCode::DuplicateIdentity,
                             "reserve block serving groups are not strictly ordered");
      }
    }
    for (SourceGroupId group : block.serving_groups) {
      if (!topology.contains_group(group)) {
        return Status::error(ErrorCode::InvalidArgument,
                             "reserve block serves an unknown source group");
      }
    }
    if (std::find(block.serving_groups.begin(), block.serving_groups.end(), block.owner_group) ==
        block.serving_groups.end()) {
      return Status::error(ErrorCode::InvalidArgument,
                           "reserve block does not list its owning group as a serving group");
    }
    topology.blocks.push_back(std::move(block));
  }
  return topology;
}

[[nodiscard]] Result<CapacityAvailabilityEvidence> read_capacity(Decoder& decoder,
                                                                 FailoverDomainId domain) {
  CapacityAvailabilityEvidence evidence;
  CF_TRY_ASSIGN(stamp, read_stamp(decoder));
  evidence.stamp = stamp;
  evidence.domain = decoder.id<FailoverDomainTag>();
  evidence.generation = decoder.generation<CapacityGenerationTag>();
  if (!decoder.ok()) {
    return decoder.status();
  }
  if (!(evidence.domain == domain)) {
    return Status::error(ErrorCode::StoreCorrupt, "capacity event belongs to another domain");
  }
  const std::size_t count = decoder.count(kMaxBlocksPerDomain);
  if (!decoder.ok()) {
    return decoder.status();
  }
  evidence.blocks.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    BlockAvailability block;
    block.block = decoder.id<ReserveBlockTag>();
    CF_TRY_ASSIGN(state, read_enum<AvailabilityState>(
                             decoder, static_cast<std::uint8_t>(AvailabilityState::Indeterminate),
                             "availability state out of range"));
    block.state = state;
    block.available = decoder.watts();
    block.observed_at = decoder.tick();
    if (!decoder.ok()) {
      return decoder.status();
    }
    if (block.block.is_nil()) {
      return Status::error(ErrorCode::InvalidArgument, "capacity block identity is nil");
    }
    if (index > 0 && !(evidence.blocks[index - 1].block < block.block)) {
      return Status::error(ErrorCode::DuplicateIdentity,
                           "capacity evidence blocks are not strictly ordered");
    }
    switch (block.state) {
      case AvailabilityState::Available:
        if (block.available.value() <= 0) {
          return Status::error(ErrorCode::InvalidArgument,
                               "available capacity must be greater than zero",
                               "block", block.block.value());
        }
        break;
      case AvailabilityState::Zero:
        if (block.available.value() != 0) {
          return Status::error(ErrorCode::InvalidArgument,
                               "zero availability must report exactly zero watts", "block",
                               block.block.value());
        }
        break;
      case AvailabilityState::Unavailable:
      case AvailabilityState::Unknown:
      case AvailabilityState::Unsupported:
      case AvailabilityState::Indeterminate:
        if (block.available.value() != 0) {
          return Status::error(ErrorCode::InvalidArgument,
                               "non-decisive availability must not carry a capacity value",
                               "block", block.block.value());
        }
        break;
    }
    evidence.blocks.push_back(block);
  }
  return evidence;
}

[[nodiscard]] Result<RedundancyPolicy> read_policy(Decoder& decoder) {
  RedundancyPolicy policy;
  CF_TRY_ASSIGN(stamp, read_stamp(decoder));
  policy.stamp = stamp;
  policy.generation = decoder.generation<PolicyGenerationTag>();
  CF_TRY_ASSIGN(mode, read_enum<SelectionMode>(decoder,
                                               static_cast<std::uint8_t>(SelectionMode::ManualOnly),
                                               "selection mode out of range"));
  policy.selection = mode;
  const std::size_t criteria = decoder.count(kMaxRankCriteria);
  if (!decoder.ok()) {
    return decoder.status();
  }
  policy.rank_order.reserve(criteria);
  for (std::size_t index = 0; index < criteria; ++index) {
    CF_TRY_ASSIGN(criterion,
                  read_enum<RankCriterion>(decoder,
                                           static_cast<std::uint8_t>(RankCriterion::OwnerGroupFirst),
                                           "rank criterion out of range"));
    policy.rank_order.push_back(criterion);
  }
  policy.minimum_independent_groups = decoder.u32();
  policy.max_groups_per_candidate = decoder.u32();
  CF_TRY_ASSIGN(margin, decoder.basis_points());
  policy.headroom_margin = margin;
  policy.recovery_consecutive_healthy = decoder.u32();
  policy.recovery_dwell_ticks = decoder.u64();
  policy.max_evidence_age_ticks = decoder.u64();
  policy.oscillation_window_ticks = decoder.u64();
  policy.max_transitions_per_window = decoder.u32();
  policy.max_plan_steps = decoder.u32();
  if (!decoder.ok()) {
    return decoder.status();
  }
  if (policy.max_groups_per_candidate > kMaxGroupsPerCandidate) {
    return Status::error(ErrorCode::BoundsExceeded, "policy allows too many groups per candidate");
  }
  if (policy.max_plan_steps > kMaxPlanSteps) {
    return Status::error(ErrorCode::BoundsExceeded, "policy allows too many plan steps");
  }
  if (policy.max_evidence_age_ticks > kMaxEvidenceAgeTicks) {
    return Status::error(ErrorCode::BoundsExceeded, "policy evidence age bound is too large");
  }
  return policy;
}

[[nodiscard]] Result<PlanStep> read_step(Decoder& decoder) {
  PlanStep step;
  step.ordinal = decoder.u32();
  step.target = decoder.id<SourceGroupTag>();
  const std::size_t count = decoder.count(static_cast<std::size_t>(EffectKind::RedundancyRestored));
  if (!decoder.ok()) {
    return decoder.status();
  }
  step.required_effects.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    CF_TRY_ASSIGN(effect, read_enum<EffectKind>(
                              decoder, static_cast<std::uint8_t>(EffectKind::RedundancyRestored),
                              "effect kind out of range"));
    step.required_effects.push_back(effect);
  }
  if (count == 0) {
    return Status::error(ErrorCode::InvalidArgument, "plan step requires no effects");
  }
  return step;
}

[[nodiscard]] Result<CommandSpec> read_command_spec(Decoder& decoder) {
  CommandSpec command;
  command.id = decoder.id<CommandTag>();
  command.key = decoder.id<IdempotencyKeyTag>();
  command.fingerprint = decoder.digest();
  command.step = decoder.u32();
  CF_TRY_ASSIGN(runtime, read_enum<OwnerSystem>(decoder,
                                                static_cast<std::uint8_t>(OwnerSystem::ActuationControllerAck),
                                                "command runtime out of range"));
  command.runtime = runtime;
  command.target = decoder.id<SourceGroupTag>();
  const std::size_t count = decoder.count(static_cast<std::size_t>(EffectKind::RedundancyRestored));
  if (!decoder.ok()) {
    return decoder.status();
  }
  command.required_effects.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    CF_TRY_ASSIGN(effect, read_enum<EffectKind>(
                              decoder, static_cast<std::uint8_t>(EffectKind::RedundancyRestored),
                              "effect kind out of range"));
    command.required_effects.push_back(effect);
  }
  if (command.id.is_nil() || command.key.is_nil()) {
    return Status::error(ErrorCode::InvalidArgument, "command identity or key is nil");
  }
  return command;
}

[[nodiscard]] Result<FailoverPlan> read_plan(Decoder& decoder) {
  FailoverPlan plan;
  plan.id = decoder.id<PlanTag>();
  plan.request = decoder.id<RequestTag>();
  plan.request_fingerprint = decoder.digest();
  plan.domain = decoder.id<FailoverDomainTag>();
  CF_TRY_ASSIGN(kind, read_enum<PlanKind>(decoder,
                                          static_cast<std::uint8_t>(PlanKind::ReturnToPrimary),
                                          "plan kind out of range"));
  plan.kind = kind;
  plan.epoch = decoder.generation<ControlPlaneEpochTag>();
  plan.revision_at_creation = decoder.counter<StateRevisionTag>();
  plan.topology = decoder.generation<TopologyGenerationTag>();
  plan.capacity = decoder.generation<CapacityGenerationTag>();
  plan.policy = decoder.generation<PolicyGenerationTag>();
  plan.evidence_generation = decoder.generation<EvidenceSetGenerationTag>();
  plan.evidence_digest = decoder.digest();
  plan.obligations_generation = decoder.u64();
  plan.incumbent = decoder.id<SourceGroupTag>();
  plan.target = decoder.id<SourceGroupTag>();
  const std::size_t group_count = decoder.count(kMaxGroupsPerCandidate);
  if (!decoder.ok()) {
    return decoder.status();
  }
  for (std::size_t index = 0; index < group_count; ++index) {
    plan.target_groups.push_back(decoder.id<SourceGroupTag>());
  }
  const std::size_t step_count = decoder.count(kMaxPlanSteps);
  if (!decoder.ok()) {
    return decoder.status();
  }
  for (std::size_t index = 0; index < step_count; ++index) {
    CF_TRY_ASSIGN(step, read_step(decoder));
    plan.steps.push_back(std::move(step));
  }
  const std::size_t command_count = decoder.count(kMaxPlanSteps * 4);
  if (!decoder.ok()) {
    return decoder.status();
  }
  for (std::size_t index = 0; index < command_count; ++index) {
    CF_TRY_ASSIGN(command, read_command_spec(decoder));
    plan.commands.push_back(std::move(command));
  }
  plan.created_at = decoder.tick();
  CF_TRY_ASSIGN(state, read_enum<PlanState>(decoder,
                                            static_cast<std::uint8_t>(PlanState::Superseded),
                                            "plan state out of range"));
  plan.state = state;
  CF_TRY_ASSIGN(reason,
                read_enum_range<FenceReason>(decoder, 0,
                                             static_cast<std::uint8_t>(FenceReason::RequestedByCaller),
                                             "fence reason out of range"));
  plan.fence_reason = reason;
  plan.fenced_at = decoder.tick();
  plan.attempts_started = decoder.u32();
  plan.reason = decoder.text();
  if (!decoder.ok()) {
    return decoder.status();
  }
  if (plan.id.is_nil()) {
    return Status::error(ErrorCode::InvalidArgument, "plan identity is nil");
  }
  if (plan.target_groups.empty()) {
    return Status::error(ErrorCode::InvalidArgument, "plan has no target groups");
  }
  if (plan.steps.empty()) {
    return Status::error(ErrorCode::InvalidArgument, "plan has no steps");
  }
  if (plan.commands.empty()) {
    return Status::error(ErrorCode::InvalidArgument, "plan has no commands");
  }
  return plan;
}

[[nodiscard]] Result<TransitionAttempt> read_attempt(Decoder& decoder) {
  TransitionAttempt attempt;
  attempt.id = decoder.id<AttemptTag>();
  attempt.plan = decoder.id<PlanTag>();
  attempt.number = decoder.u32();
  attempt.epoch = decoder.generation<ControlPlaneEpochTag>();
  CF_TRY_ASSIGN(state, read_enum<AttemptState>(decoder,
                                               static_cast<std::uint8_t>(AttemptState::Refused),
                                               "attempt state out of range"));
  attempt.state = state;
  attempt.created_at = decoder.tick();
  attempt.updated_at = decoder.tick();
  attempt.revision_at_issue = decoder.counter<StateRevisionTag>();
  attempt.plan_binding = decoder.digest();
  const std::size_t command_count = decoder.count(kMaxPlanSteps * 4);
  if (!decoder.ok()) {
    return decoder.status();
  }
  for (std::size_t index = 0; index < command_count; ++index) {
    attempt.commands.push_back(decoder.id<CommandTag>());
  }
  const std::size_t verified_count =
      decoder.count(static_cast<std::size_t>(EffectKind::RedundancyRestored));
  if (!decoder.ok()) {
    return decoder.status();
  }
  for (std::size_t index = 0; index < verified_count; ++index) {
    CF_TRY_ASSIGN(effect, read_enum<EffectKind>(
                              decoder, static_cast<std::uint8_t>(EffectKind::RedundancyRestored),
                              "effect kind out of range"));
    attempt.verified_effects.push_back(effect);
  }
  const std::size_t missing_count =
      decoder.count(static_cast<std::size_t>(EffectKind::RedundancyRestored));
  if (!decoder.ok()) {
    return decoder.status();
  }
  for (std::size_t index = 0; index < missing_count; ++index) {
    CF_TRY_ASSIGN(effect, read_enum<EffectKind>(
                              decoder, static_cast<std::uint8_t>(EffectKind::RedundancyRestored),
                              "effect kind out of range"));
    attempt.missing_effects.push_back(effect);
  }
  attempt.commands_issued = decoder.u32();
  attempt.commands_replayed = decoder.u32();
  attempt.commands_refused = decoder.u32();
  attempt.observations_accepted = decoder.u32();
  attempt.detail = decoder.text();
  if (!decoder.ok()) {
    return decoder.status();
  }
  if (attempt.id.is_nil() || attempt.plan.is_nil()) {
    return Status::error(ErrorCode::InvalidArgument, "attempt identity or plan is nil");
  }
  return attempt;
}

void refresh_evidence(DomainState& state, Tick now) {
  state.touch_evidence(now);
}

}  // namespace

Result<void> apply_event(DomainState& state, RecordType type, std::span<const std::uint8_t> payload,
                         bool recovering) {
  Decoder decoder(payload);
  Tick event_tick{};
  switch (type) {
    case RecordType::DomainRegistered: {
      event_tick = decoder.tick();
      CF_TRY(decoder.require_end("domain registration"));
      state.registered = true;
      state.registered_at = event_tick;
      break;
    }

    case RecordType::EpochEstablished: {
      const ControlPlaneEpoch epoch = decoder.generation<ControlPlaneEpochTag>();
      event_tick = decoder.tick();
      CF_TRY(decoder.require_end("epoch establishment"));
      if (!epoch.is_set()) {
        return Status::error(ErrorCode::InvalidArgument, "epoch must be set");
      }
      state.epoch = epoch;
      state.epoch_established = true;
      break;
    }

    case RecordType::TopologyImported: {
      CF_TRY_ASSIGN(topology, read_topology(decoder, state.domain));
      CF_TRY(decoder.require_end("topology import"));
      event_tick = topology.stamp.observed_at;
      state.topology = std::move(topology);
      state.has_topology = true;
      refresh_evidence(state, event_tick);
      break;
    }

    case RecordType::CapacityRecorded: {
      CF_TRY_ASSIGN(evidence, read_capacity(decoder, state.domain));
      CF_TRY(decoder.require_end("capacity evidence"));
      event_tick = evidence.stamp.observed_at;
      state.capacity = std::move(evidence);
      state.has_capacity = true;
      refresh_evidence(state, event_tick);
      break;
    }

    case RecordType::HealthRecorded: {
      SourceHealthEvidence evidence;
      CF_TRY_ASSIGN(stamp, read_stamp(decoder));
      evidence.stamp = stamp;
      evidence.source = decoder.id<CoolingSourceTag>();
      CF_TRY_ASSIGN(health, read_enum<HealthState>(decoder,
                                                   static_cast<std::uint8_t>(HealthState::Unsupported),
                                                   "health state out of range"));
      evidence.state = health;
      CF_TRY(decoder.require_end("health evidence"));
      if (evidence.source.is_nil()) {
        return Status::error(ErrorCode::InvalidArgument, "health evidence source identity is nil");
      }
      event_tick = evidence.stamp.observed_at;
      state.health[evidence.source] = evidence;
      if (recovering) {
        // Health streaks are dynamic observations. Recovered state must re-earn
        // them from current evidence before recovery can be eligible.
        state.streaks.clear();
      } else if (state.has_topology) {
        for (const SourceGroupDescriptor& group : state.topology.groups) {
          const std::vector<CoolingSourceId> members = state.topology.members_of(group.group);
          if (std::find(members.begin(), members.end(), evidence.source) == members.end()) {
            continue;
          }
          HealthStreak& streak = state.streaks[group.group];
          const bool generation_changed =
              !(streak.topology == state.topology.generation) ||
              !(streak.capacity == state.capacity.generation);
          if (evidence.state == HealthState::Healthy) {
            if (generation_changed || streak.consecutive_healthy == 0) {
              streak.consecutive_healthy = 1;
              streak.streak_started_at = evidence.stamp.observed_at;
            } else {
              ++streak.consecutive_healthy;
            }
          } else {
            streak.consecutive_healthy = 0;
            streak.streak_started_at = evidence.stamp.observed_at;
          }
          streak.group = group.group;
          streak.last_observed_at = evidence.stamp.observed_at;
          streak.topology = state.topology.generation;
          streak.capacity = state.capacity.generation;
        }
      }
      refresh_evidence(state, event_tick);
      break;
    }

    case RecordType::AuthorityRecorded: {
      ServiceAuthorityEvidence evidence;
      CF_TRY_ASSIGN(stamp, read_stamp(decoder));
      evidence.stamp = stamp;
      evidence.domain = decoder.id<FailoverDomainTag>();
      evidence.epoch = decoder.generation<ControlPlaneEpochTag>();
      CF_TRY_ASSIGN(authority, read_enum<AuthorityState>(
                                   decoder, static_cast<std::uint8_t>(AuthorityState::Superseded),
                                   "authority state out of range"));
      evidence.state = authority;
      CF_TRY(decoder.require_end("authority evidence"));
      if (!(evidence.domain == state.domain)) {
        return Status::error(ErrorCode::StoreCorrupt, "authority event belongs to another domain");
      }
      event_tick = evidence.stamp.observed_at;
      state.authority = evidence;
      state.has_authority = true;
      refresh_evidence(state, event_tick);
      break;
    }

    case RecordType::PolicyInstalled: {
      CF_TRY_ASSIGN(policy, read_policy(decoder));
      CF_TRY(decoder.require_end("redundancy policy"));
      event_tick = policy.stamp.observed_at;
      state.policy = std::move(policy);
      state.has_policy = true;
      refresh_evidence(state, event_tick);
      break;
    }

    case RecordType::ObligationsInstalled: {
      ObligationSet obligations;
      CF_TRY_ASSIGN(stamp, read_stamp(decoder));
      obligations.stamp = stamp;
      obligations.generation = decoder.u64();
      const std::size_t count = decoder.count(kMaxObligationsPerDomain);
      if (!decoder.ok()) {
        return decoder.status();
      }
      obligations.obligations.reserve(count);
      for (std::size_t index = 0; index < count; ++index) {
        ProtectedObligation obligation;
        obligation.id = decoder.id<ObligationTag>();
        obligation.required_capacity = decoder.watts();
        obligation.required_independent_groups = decoder.u32();
        CF_TRY_ASSIGN(criticality, read_enum<Criticality>(
                                       decoder, static_cast<std::uint8_t>(Criticality::BestEffort),
                                       "criticality out of range"));
        obligation.criticality = criticality;
        obligation.reference = decoder.text();
        if (!decoder.ok()) {
          return decoder.status();
        }
        if (obligation.id.is_nil()) {
          return Status::error(ErrorCode::InvalidArgument, "obligation identity is nil");
        }
        if (obligation.required_capacity.is_negative()) {
          return Status::error(ErrorCode::InvalidArgument,
                               "obligation requires a negative capacity");
        }
        if (obligation.required_independent_groups == 0 ||
            obligation.required_independent_groups > kMaxGroupsPerCandidate) {
          return Status::error(ErrorCode::OutOfRange,
                               "obligation independence requirement is out of range");
        }
        if (index > 0 && !(obligations.obligations[index - 1].id < obligation.id)) {
          return Status::error(ErrorCode::DuplicateIdentity,
                               "protected obligations are not strictly ordered");
        }
        obligations.obligations.push_back(std::move(obligation));
      }
      CF_TRY(decoder.require_end("protected obligations"));
      event_tick = obligations.stamp.observed_at;
      state.obligations = std::move(obligations);
      state.has_obligations = true;
      refresh_evidence(state, event_tick);
      break;
    }

    case RecordType::PlanCreated: {
      CF_TRY_ASSIGN(plan, read_plan(decoder));
      CF_TRY(decoder.require_end("plan creation"));
      if (!(plan.domain == state.domain)) {
        return Status::error(ErrorCode::StoreCorrupt, "plan belongs to another domain");
      }
      event_tick = plan.created_at;
      if (!plan.request.is_nil()) {
        state.plan_requests[plan.request] = std::make_pair(plan.request_fingerprint, plan.id);
      }
      if (plan.state == PlanState::Active) {
        state.active_plan = plan.id;
      }
      state.plans[plan.id] = std::move(plan);
      break;
    }

    case RecordType::PlanStateChanged: {
      const PlanId plan_id = decoder.id<PlanTag>();
      CF_TRY_ASSIGN(plan_state, read_enum<PlanState>(
                                    decoder, static_cast<std::uint8_t>(PlanState::Superseded),
                                    "plan state out of range"));
      CF_TRY_ASSIGN(reason, read_enum_range<FenceReason>(
                                decoder, 0,
                                static_cast<std::uint8_t>(FenceReason::RequestedByCaller),
                                "fence reason out of range"));
      event_tick = decoder.tick();
      CF_TRY(decoder.require_end("plan state change"));
      const auto it = state.plans.find(plan_id);
      if (it == state.plans.end()) {
        return Status::error(ErrorCode::StoreCorrupt, "plan state change references an unknown plan");
      }
      it->second.state = plan_state;
      it->second.fence_reason = reason;
      it->second.fenced_at = event_tick;
      if (state.active_plan == plan_id) {
        state.active_plan = PlanId::nil();
        state.active_attempt = AttemptId::nil();
      }
      break;
    }

    case RecordType::CommandRecorded: {
      CommandRecord command;
      command.id = decoder.id<CommandTag>();
      command.key = decoder.id<IdempotencyKeyTag>();
      command.fingerprint = decoder.digest();
      command.plan = decoder.id<PlanTag>();
      command.attempt = decoder.id<AttemptTag>();
      command.step = decoder.u32();
      CF_TRY_ASSIGN(runtime, read_enum<OwnerSystem>(
                                 decoder,
                                 static_cast<std::uint8_t>(OwnerSystem::ActuationControllerAck),
                                 "command runtime out of range"));
      command.runtime = runtime;
      command.target = decoder.id<SourceGroupTag>();
      CF_TRY_ASSIGN(outcome, read_enum<CommandOutcome>(
                                 decoder, static_cast<std::uint8_t>(CommandOutcome::Rejected),
                                 "command outcome out of range"));
      command.outcome = outcome;
      command.issue_sequence = decoder.counter<IssueSequenceTag>();
      event_tick = decoder.tick();
      command.issued_at = event_tick;
      command.epoch = decoder.generation<ControlPlaneEpochTag>();
      command.detail = decoder.text();
      CF_TRY(decoder.require_end("command record"));
      if (command.id.is_nil() || command.key.is_nil()) {
        return Status::error(ErrorCode::InvalidArgument, "command identity or key is nil");
      }
      const auto existing = state.idempotency.find(command.key);
      if (existing != state.idempotency.end()) {
        if (!(existing->second.fingerprint == command.fingerprint)) {
          return Status::error(ErrorCode::StoreCorrupt,
                               "idempotency key was recorded with a different fingerprint");
        }
      }
      state.commands[command.id] = command;
      IdempotencyRecord record;
      record.key = command.key;
      record.fingerprint = command.fingerprint;
      record.outcome = command.outcome;
      record.command = command.id;
      record.detail = command.detail;
      state.idempotency[command.key] = std::move(record);
      break;
    }

    case RecordType::ObservationRecorded: {
      EffectObservationRecord observation;
      observation.id = decoder.id<ObservationTag>();
      observation.command = decoder.id<CommandTag>();
      observation.group = decoder.id<SourceGroupTag>();
      CF_TRY_ASSIGN(effect, read_enum<EffectKind>(
                                decoder, static_cast<std::uint8_t>(EffectKind::RedundancyRestored),
                                "effect kind out of range"));
      observation.effect = effect;
      CF_TRY_ASSIGN(origin, read_enum<ObservationOrigin>(
                                decoder, static_cast<std::uint8_t>(ObservationOrigin::ActuationControllerAck),
                                "observation origin out of range"));
      observation.origin = origin;
      observation.sequence = decoder.counter<EffectSequenceTag>();
      observation.topology = decoder.generation<TopologyGenerationTag>();
      observation.capacity = decoder.generation<CapacityGenerationTag>();
      observation.epoch = decoder.generation<ControlPlaneEpochTag>();
      event_tick = decoder.tick();
      observation.observed_at = event_tick;
      const bool stored_recovered = decoder.boolean();
      CF_TRY(decoder.require_end("effect observation"));
      if (observation.id.is_nil()) {
        return Status::error(ErrorCode::InvalidArgument, "observation identity is nil");
      }
      if (recovering) {
        observation.recovered_unvalidated = true;
      } else {
        observation.recovered_unvalidated = stored_recovered;
      }
      if (state.observations.find(observation.id) == state.observations.end()) {
        state.observation_order.push_back(observation.id);
      }
      state.observations[observation.id] = observation;
      while (state.observation_order.size() > kMaxRetainedObservations) {
        const ObservationId oldest = state.observation_order.front();
        state.observation_order.erase(state.observation_order.begin());
        state.observations.erase(oldest);
      }
      break;
    }

    case RecordType::AttemptRecorded: {
      CF_TRY_ASSIGN(attempt, read_attempt(decoder));
      CF_TRY(decoder.require_end("transition attempt"));
      event_tick = attempt.updated_at;
      if (recovering && !attempt_state_is_terminal(attempt.state)) {
        attempt.state = AttemptState::Interrupted;
        attempt.detail = "recovered from durable state; re-verification required";
      }
      if (attempt_state_is_terminal(attempt.state)) {
        if (state.active_attempt == attempt.id) {
          state.active_attempt = AttemptId::nil();
        }
      } else {
        state.active_attempt = attempt.id;
        state.active_plan = attempt.plan;
      }
      state.attempts[attempt.id] = std::move(attempt);
      break;
    }

    case RecordType::TransitionRecorded: {
      TransitionRecord record;
      record.plan = decoder.id<PlanTag>();
      CF_TRY_ASSIGN(kind, read_enum<PlanKind>(decoder,
                                              static_cast<std::uint8_t>(PlanKind::ReturnToPrimary),
                                              "plan kind out of range"));
      record.kind = kind;
      record.from_group = decoder.id<SourceGroupTag>();
      record.to_group = decoder.id<SourceGroupTag>();
      event_tick = decoder.tick();
      record.at = event_tick;
      CF_TRY(decoder.require_end("transition record"));
      state.transitions.push_back(record);
      while (state.transitions.size() > kMaxTransitionHistory) {
        state.transitions.erase(state.transitions.begin());
      }
      break;
    }
  }

  CF_TRY_ASSIGN(next_revision, state.revision.next());
  state.revision = next_revision;
  if (event_tick > state.last_tick) {
    state.last_tick = event_tick;
  }
  return Result<void>();
}

}  // namespace cooling_failover
