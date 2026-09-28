#include "cooling_failover/orchestrator.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <utility>

namespace cooling_failover {
namespace {

[[nodiscard]] std::uint64_t digest_word(const Digest& digest, std::size_t offset) noexcept {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(digest.bytes()[offset + index]) << (8U * index);
  }
  return value == 0 ? 1 : value;  // identity 0 means "absent"
}

[[nodiscard]] bool contains_effect(const std::vector<EffectKind>& effects, EffectKind effect) {
  return std::find(effects.begin(), effects.end(), effect) != effects.end();
}

[[nodiscard]] std::vector<OwnerSystem> runtimes_of_group(const TopologyProjection& topology,
                                                        SourceGroupId group) {
  std::set<OwnerSystem> runtimes;
  for (const SourceDescriptor& source : topology.sources) {
    if (source.group == group) {
      runtimes.insert(actuation_runtime_for(source.kind));
    }
  }
  if (runtimes.empty()) {
    runtimes.insert(OwnerSystem::LiquidCoolingControl);
  }
  return std::vector<OwnerSystem>(runtimes.begin(), runtimes.end());
}

[[nodiscard]] std::vector<EffectKind> effects_for_group(const DomainState& state,
                                                        std::size_t group_index,
                                                        std::size_t group_count) {
  std::vector<EffectKind> effects{EffectKind::FlowEstablished, EffectKind::CapacityDelivered};
  if (state.has_obligations) {
    for (const ProtectedObligation& obligation : state.obligations.obligations) {
      if (obligation.criticality == Criticality::Critical) {
        effects.push_back(EffectKind::ReturnTemperatureInBand);
        break;
      }
    }
  }
  if (state.has_policy && group_index + 1 == group_count &&
      group_count >= state.policy.minimum_independent_groups && group_count >= 2) {
    effects.push_back(EffectKind::RedundancyRestored);
  }
  std::sort(effects.begin(), effects.end());
  effects.erase(std::unique(effects.begin(), effects.end()), effects.end());
  return effects;
}

[[nodiscard]] Digest request_fingerprint(const PlanRequest& request) {
  Encoder encoder;
  encoder.u32(1);
  encoder.u8(static_cast<std::uint8_t>(request.kind));
  encoder.id(request.incumbent);
  encoder.id(request.target);
  encoder.text(request.reason);
  return encoder.finish_digest();
}

[[nodiscard]] Digest plan_fingerprint(const PlanRequest& request, const CandidateKey& key,
                                      ControlPlaneEpoch epoch, Tick now) {
  Encoder encoder;
  encoder.u32(1);
  encoder.id(request.request);
  encoder.id(request.incumbent);
  encoder.u8(static_cast<std::uint8_t>(request.kind));
  encoder.generation(epoch);
  encoder.collection(key.groups);
  for (SourceGroupId group : key.groups) {
    encoder.id(group);
  }
  encoder.tick(now);
  return encoder.finish_digest();
}

[[nodiscard]] IdempotencyKey derive_key(PlanId plan, std::uint32_t step, OwnerSystem runtime) {
  Encoder encoder;
  encoder.u32(1);
  encoder.id(plan);
  encoder.u32(step);
  encoder.u8(static_cast<std::uint8_t>(runtime));
  return IdempotencyKey::from_value(digest_word(encoder.finish_digest(), 0));
}

[[nodiscard]] CommandId derive_command_id(PlanId plan, std::uint32_t step, OwnerSystem runtime) {
  Encoder encoder;
  encoder.u32(2);
  encoder.id(plan);
  encoder.u32(step);
  encoder.u8(static_cast<std::uint8_t>(runtime));
  return CommandId::from_value(digest_word(encoder.finish_digest(), 0));
}

[[nodiscard]] AttemptId derive_attempt_id(PlanId plan, std::uint32_t number) {
  Encoder encoder;
  encoder.u32(3);
  encoder.id(plan);
  encoder.u32(number);
  return AttemptId::from_value(digest_word(encoder.finish_digest(), 0));
}

[[nodiscard]] Digest command_fingerprint(PlanId plan, std::uint32_t step, OwnerSystem runtime,
                                         SourceGroupId target,
                                         const std::vector<EffectKind>& effects) {
  Encoder encoder;
  encoder.u32(4);
  encoder.id(plan);
  encoder.u32(step);
  encoder.u8(static_cast<std::uint8_t>(runtime));
  encoder.id(target);
  encoder.collection(effects);
  for (EffectKind effect : effects) {
    encoder.u8(static_cast<std::uint8_t>(effect));
  }
  return encoder.finish_digest();
}

[[nodiscard]] bool command_outcome_counts_as_acknowledged(CommandOutcome outcome) noexcept {
  return outcome == CommandOutcome::Acknowledged || outcome == CommandOutcome::Replayed;
}

[[nodiscard]] bool command_outcome_counts_as_refused(CommandOutcome outcome) noexcept {
  return outcome == CommandOutcome::Refused || outcome == CommandOutcome::Rejected ||
         outcome == CommandOutcome::RuntimeUnavailable || outcome == CommandOutcome::NotIssued;
}

}  // namespace

ControlRuntimePort::~ControlRuntimePort() = default;

void ControlRuntimeRegistry::set(OwnerSystem runtime, ControlRuntimePort* port) noexcept {
  switch (runtime) {
    case OwnerSystem::AirflowControl:
      airflow_ = port;
      break;
    case OwnerSystem::LiquidCoolingControl:
      liquid_ = port;
      break;
    default:
      break;
  }
}

ControlRuntimePort* ControlRuntimeRegistry::find(OwnerSystem runtime) const noexcept {
  switch (runtime) {
    case OwnerSystem::AirflowControl:
      return airflow_;
    case OwnerSystem::LiquidCoolingControl:
      return liquid_;
    default:
      return nullptr;
  }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Result<std::unique_ptr<CoolingFailoverOrchestrator>> CoolingFailoverOrchestrator::open(
    OrchestratorOptions options, FailoverDomainId domain) {
  if (domain.is_nil()) {
    return Status::error(ErrorCode::InvalidArgument, "failover domain identity is nil");
  }
  CF_TRY_ASSIGN(store, DurableStore::open(options.store, domain));
  auto orchestrator = std::unique_ptr<CoolingFailoverOrchestrator>(new CoolingFailoverOrchestrator());
  CF_TRY(orchestrator->resume(std::move(store), options));
  return orchestrator;
}

Result<void> CoolingFailoverOrchestrator::resume(std::unique_ptr<DurableStore> store,
                                                 OrchestratorOptions options) {
  options_ = options;
  domain_ = store->domain();
  store_ = std::move(store);
  state_ = DomainState{};
  state_.domain = domain_;

  if (store_->has_snapshot()) {
    CF_TRY_ASSIGN(events, decode_event_stream(store_->snapshot_payload()));
    for (const auto& entry : events) {
      CF_TRY(apply_event(state_, entry.first, entry.second, true));
    }
  }
  for (const auto& entry : store_->records()) {
    CF_TRY(apply_event(state_, entry.first, entry.second, true));
  }
  state_.last_commit = store_->last_commit();
  state_.revision = StateRevision::from_value(state_.last_commit.value());
  state_.recovered = true;
  interrupt_recovered_attempts();
  CF_TRY(state_.validate());
  return Result<void>();
}

Result<void> CoolingFailoverOrchestrator::rebuild_from_store() {
  DomainState rebuilt;
  rebuilt.domain = domain_;
  if (store_->has_snapshot()) {
    CF_TRY_ASSIGN(events, decode_event_stream(store_->snapshot_payload()));
    for (const auto& entry : events) {
      CF_TRY(apply_event(rebuilt, entry.first, entry.second, true));
    }
  }
  for (const auto& entry : store_->records()) {
    CF_TRY(apply_event(rebuilt, entry.first, entry.second, true));
  }
  rebuilt.last_commit = store_->last_commit();
  rebuilt.revision = StateRevision::from_value(rebuilt.last_commit.value());
  rebuilt.recovered = true;
  state_ = std::move(rebuilt);
  interrupt_recovered_attempts();
  return Result<void>();
}

void CoolingFailoverOrchestrator::interrupt_recovered_attempts() {
  state_.active_plan = PlanId::nil();
  state_.active_attempt = AttemptId::nil();
  for (auto& entry : state_.plans) {
    if (entry.second.state != PlanState::Active) {
      continue;
    }
    if (state_.active_plan.is_nil() || entry.first < state_.active_plan) {
      state_.active_plan = entry.first;
    }
  }
  if (state_.active_plan.is_nil()) {
    return;
  }
  for (auto& entry : state_.attempts) {
    if (!(entry.second.plan == state_.active_plan)) {
      continue;
    }
    if (attempt_state_is_terminal(entry.second.state)) {
      continue;
    }
    entry.second.state = AttemptState::Interrupted;
    entry.second.detail = "recovered from durable state; re-verification required";
    state_.active_attempt = entry.first;
  }
}

Result<void> CoolingFailoverOrchestrator::close() {
  if (closed_) {
    return Result<void>();
  }
  closed_ = true;
  store_.reset();
  return Result<void>();
}

CoolingFailoverOrchestrator::~CoolingFailoverOrchestrator() { closed_ = true; }

const RecoveryOutcome& CoolingFailoverOrchestrator::recovery() const noexcept {
  static const RecoveryOutcome kEmpty{};
  return store_ != nullptr ? store_->recovery() : kEmpty;
}

void CoolingFailoverOrchestrator::set_control_runtime(OwnerSystem runtime,
                                                      ControlRuntimePort* port) noexcept {
  runtimes_.set(runtime, port);
}

Result<void> CoolingFailoverOrchestrator::require_open() const {
  if (closed_ || store_ == nullptr) {
    return Status::error(ErrorCode::StoreClosed, "orchestrator is closed");
  }
  return Result<void>();
}

Result<void> CoolingFailoverOrchestrator::commit(RecordType type,
                                                 std::span<const std::uint8_t> payload) {
  CF_TRY(require_open());
  CF_TRY(apply_event(state_, type, payload, false));
  Result<CommitSequence> appended = store_->append(type, payload);
  if (!appended.has_value()) {
    const Status failure = appended.status();
    // Durable state and in-memory state must not diverge: re-derive from disk.
    Result<void> rebuilt = rebuild_from_store();
    if (!rebuilt.has_value()) {
      closed_ = true;
      return Status::error(ErrorCode::InternalError,
                           "in-memory state could not be rebuilt after a durable write failure");
    }
    return failure;
  }
  state_.last_commit = *appended;
  // The state revision is the durable commit sequence: a snapshot compaction
  // changes how many events are replayed, never the revision of the state.
  state_.revision = StateRevision::from_value(appended->value());
  return Result<void>();
}

// ---------------------------------------------------------------------------
// Binding and fencing
// ---------------------------------------------------------------------------

Result<Digest> CoolingFailoverOrchestrator::binding_evidence_digest(
    const FailoverPlan& plan) const {
  return compute_evidence_digest(state_, plan.target_groups, plan.incumbent);
}

Result<void> CoolingFailoverOrchestrator::assert_plan_live(const FailoverPlan& plan) const {
  if (plan.state != PlanState::Active) {
    return Status::error(ErrorCode::PlanFenced, "plan is no longer active", "plan",
                         plan.id.value(), "state", to_string(plan.state));
  }
  if (!state_.epoch_established) {
    return Status::error(ErrorCode::EpochNotEstablished,
                         "no control-plane epoch has been established");
  }
  if (!(state_.epoch == plan.epoch)) {
    return Status::error(ErrorCode::SupersededEpoch, "a newer control-plane epoch supersedes the plan",
                         "plan_epoch", plan.epoch.value(), "current_epoch", state_.epoch.value());
  }
  if (!state_.has_topology) {
    return Status::error(ErrorCode::TopologyNotImported, "no topology projection is available");
  }
  if (!(state_.topology.generation == plan.topology)) {
    return Status::error(ErrorCode::StaleGeneration, "topology generation changed since planning",
                         "generation", "topology", "plan", plan.topology.value(), "current",
                         state_.topology.generation.value());
  }
  if (!state_.has_capacity || !(state_.capacity.generation == plan.capacity)) {
    return Status::error(ErrorCode::StaleGeneration, "capacity generation changed since planning",
                         "generation", "capacity", "plan", plan.capacity.value(), "current",
                         state_.capacity.generation.value());
  }
  if (!state_.has_policy || !(state_.policy.generation == plan.policy)) {
    return Status::error(ErrorCode::StaleGeneration, "policy generation changed since planning",
                         "generation", "policy", "plan", plan.policy.value(), "current",
                         state_.policy.generation.value());
  }
  if (!state_.has_obligations || state_.obligations.generation != plan.obligations_generation) {
    return Status::error(ErrorCode::StaleGeneration,
                         "protected obligations changed since planning", "generation",
                         "obligations", "plan", plan.obligations_generation,
                         "current", state_.has_obligations ? state_.obligations.generation : 0);
  }
  if (!state_.has_authority) {
    return Status::error(ErrorCode::AuthorityMissing, "no service authority evidence is current");
  }
  if (!(state_.authority.epoch == plan.epoch)) {
    return Status::error(ErrorCode::EpochMismatch,
                         "service authority does not cover the plan epoch", "authority_epoch",
                         state_.authority.epoch.value(), "plan_epoch", plan.epoch.value());
  }
  switch (state_.authority.state) {
    case AuthorityState::Granted:
      break;
    case AuthorityState::Denied:
      return Status::error(ErrorCode::AuthorityDenied,
                           "service authority for this domain has been denied");
    case AuthorityState::Superseded:
      return Status::error(ErrorCode::AuthoritySuperseded,
                           "service authority for this domain has been superseded");
    case AuthorityState::Unknown:
      return Status::error(ErrorCode::AuthorityUnknown,
                           "service authority for this domain is unknown");
  }
  CF_TRY_ASSIGN(current, binding_evidence_digest(plan));
  if (!(current == plan.evidence_digest)) {
    return Status::error(ErrorCode::EvidenceGenerationMismatch,
                         "the evidence bound by the plan has changed", "plan",
                         plan.id.value());
  }
  return Result<void>();
}

FenceReason CoolingFailoverOrchestrator::fence_reason_for(const Status& status) noexcept {
  switch (status.code()) {
    case ErrorCode::SupersededEpoch:
    case ErrorCode::EpochMismatch:
    case ErrorCode::EpochNotEstablished:
      return FenceReason::EpochSuperseded;
    case ErrorCode::AuthorityDenied:
    case ErrorCode::AuthoritySuperseded:
    case ErrorCode::AuthorityUnknown:
    case ErrorCode::AuthorityMissing:
      return FenceReason::AuthorityLost;
    case ErrorCode::StaleGeneration:
      for (const auto& entry : status.context()) {
        if (entry.first == "generation") {
          if (entry.second == "topology") {
            return FenceReason::TopologyGenerationChanged;
          }
          if (entry.second == "capacity") {
            return FenceReason::CapacityGenerationChanged;
          }
          if (entry.second == "policy") {
            return FenceReason::PolicyGenerationChanged;
          }
        }
      }
      return FenceReason::EvidenceChanged;
    case ErrorCode::EvidenceGenerationMismatch:
      return FenceReason::EvidenceChanged;
    case ErrorCode::PlanFenced:
      return FenceReason::Manual;
    default:
      return FenceReason::EvidenceChanged;
  }
}

Result<void> CoolingFailoverOrchestrator::fence_plan_internal(PlanId plan, FenceReason reason,
                                                              Tick now) {
  const auto it = state_.plans.find(plan);
  if (it == state_.plans.end()) {
    return Status::error(ErrorCode::PlanNotFound, "plan is unknown", "plan", plan.value());
  }
  if (it->second.state != PlanState::Active) {
    return Status::error(ErrorCode::PlanNotActive, "plan is not active", "plan", plan.value(),
                         "state", to_string(it->second.state));
  }
  return commit(RecordType::PlanStateChanged,
                encode_plan_state(plan, PlanState::Fenced, reason, now));
}

Result<void> CoolingFailoverOrchestrator::enforce_bindings(Tick now) {
  std::vector<PlanId> active;
  for (const auto& entry : state_.plans) {
    if (entry.second.state == PlanState::Active) {
      active.push_back(entry.first);
    }
  }
  for (PlanId id : active) {
    const auto it = state_.plans.find(id);
    if (it == state_.plans.end() || it->second.state != PlanState::Active) {
      continue;
    }
    Result<void> live = assert_plan_live(it->second);
    if (live.has_value()) {
      continue;
    }
    const FenceReason reason = fence_reason_for(live.status());
    std::vector<AttemptId> in_flight;
    for (const auto& attempt : state_.attempts) {
      if ((attempt.second.plan == id) && !attempt_state_is_terminal(attempt.second.state)) {
        in_flight.push_back(attempt.first);
      }
    }
    CF_TRY(fence_plan_internal(id, reason, now));
    // Every attempt that was in flight for the fenced plan is fenced with it.
    for (AttemptId attempt_id : in_flight) {
      TransitionAttempt attempt = state_.attempts.at(attempt_id);
      attempt.state = AttemptState::Fenced;
      attempt.updated_at = now;
      attempt.detail = std::string("plan fenced: ") + to_string(reason);
      CF_TRY(update_attempt(std::move(attempt)));
    }
  }
  return Result<void>();
}

Result<void> CoolingFailoverOrchestrator::update_attempt(TransitionAttempt attempt) {
  const AttemptId id = attempt.id;
  CF_TRY(commit(RecordType::AttemptRecorded, encode_attempt(attempt)));
  if (!state_.attempts.count(id)) {
    return Status::error(ErrorCode::InternalError, "attempt record did not apply");
  }
  return Result<void>();
}

Result<FailoverPlan> CoolingFailoverOrchestrator::fence_plan(PlanId plan, FenceReason reason,
                                                             Tick now) {
  CF_TRY(require_open());
  CF_TRY(fence_plan_internal(plan, reason, now));
  const auto it = state_.plans.find(plan);
  if (it == state_.plans.end()) {
    return Status::error(ErrorCode::PlanNotFound, "plan is unknown", "plan", plan.value());
  }
  return it->second;
}

// ---------------------------------------------------------------------------
// Imported evidence
// ---------------------------------------------------------------------------

Result<StateRevision> CoolingFailoverOrchestrator::register_domain(Tick now) {
  CF_TRY(require_open());
  if (state_.registered) {
    return Status::error(ErrorCode::DuplicateIdentity, "domain is already registered", "domain",
                         domain_.value());
  }
  CF_TRY(commit(RecordType::DomainRegistered, encode_domain_registered(now)));
  return state_.revision;
}

Result<StateRevision> CoolingFailoverOrchestrator::establish_epoch(ControlPlaneEpoch epoch,
                                                                   Tick now) {
  CF_TRY(require_open());
  if (!epoch.is_set()) {
    return Status::error(ErrorCode::InvalidArgument, "epoch must be set");
  }
  if (state_.epoch_established && epoch < state_.epoch) {
    return Status::error(ErrorCode::SupersededEpoch, "epoch is older than the established epoch",
                         "requested", epoch.value(), "current", state_.epoch.value());
  }
  if (!state_.has_authority) {
    return Status::error(ErrorCode::AuthorityMissing,
                         "epoch cannot be established without current authority evidence");
  }
  if (!(state_.authority.epoch == epoch)) {
    return Status::error(ErrorCode::EpochMismatch,
                         "authority evidence does not cover the requested epoch", "authority_epoch",
                         state_.authority.epoch.value(), "requested", epoch.value());
  }
  switch (state_.authority.state) {
    case AuthorityState::Granted:
      break;
    case AuthorityState::Denied:
      return Status::error(ErrorCode::AuthorityDenied, "service authority has been denied");
    case AuthorityState::Superseded:
      return Status::error(ErrorCode::AuthoritySuperseded, "service authority has been superseded");
    case AuthorityState::Unknown:
      return Status::error(ErrorCode::AuthorityUnknown, "service authority is unknown");
  }
  if (state_.epoch_established && state_.epoch == epoch) {
    return state_.revision;  // already established; idempotent
  }
  // Advancing the epoch supersedes every plan bound to the previous epoch.
  std::vector<PlanId> active;
  for (const auto& entry : state_.plans) {
    if (entry.second.state == PlanState::Active) {
      active.push_back(entry.first);
    }
  }
  for (PlanId id : active) {
    CF_TRY(fence_plan_internal(id, FenceReason::EpochSuperseded, now));
  }
  CF_TRY(commit(RecordType::EpochEstablished, encode_epoch_established(epoch, now)));
  return state_.revision;
}

Result<StateRevision> CoolingFailoverOrchestrator::import_topology(TopologyProjection projection) {
  CF_TRY(require_open());
  if (!(projection.domain == domain_)) {
    return Status::error(ErrorCode::InvalidArgument, "topology projection belongs to another domain",
                         "domain", projection.domain.value());
  }
  if (projection.stamp.evidence.is_nil()) {
    return Status::error(ErrorCode::InvalidArgument, "topology evidence identity is nil");
  }
  if (!projection.generation.is_set()) {
    return Status::error(ErrorCode::InvalidArgument, "topology generation must be set");
  }
  if (projection.groups.size() > kMaxGroupsPerDomain) {
    return Status::error(ErrorCode::BoundsExceeded, "topology has too many source groups", "count",
                         static_cast<std::uint64_t>(projection.groups.size()));
  }
  if (projection.sources.size() > kMaxSourcesPerDomain) {
    return Status::error(ErrorCode::BoundsExceeded, "topology has too many sources", "count",
                         static_cast<std::uint64_t>(projection.sources.size()));
  }
  if (projection.blocks.size() > kMaxBlocksPerDomain) {
    return Status::error(ErrorCode::BoundsExceeded, "topology has too many reserve blocks", "count",
                         static_cast<std::uint64_t>(projection.blocks.size()));
  }
  for (std::size_t index = 1; index < projection.groups.size(); ++index) {
    if (!(projection.groups[index - 1].group < projection.groups[index].group)) {
      return Status::error(ErrorCode::DuplicateIdentity,
                           "topology groups must be strictly ascending and unique");
    }
  }
  for (std::size_t index = 1; index < projection.sources.size(); ++index) {
    if (!(projection.sources[index - 1].source < projection.sources[index].source)) {
      return Status::error(ErrorCode::DuplicateIdentity,
                           "topology sources must be strictly ascending and unique");
    }
  }
  for (std::size_t index = 1; index < projection.blocks.size(); ++index) {
    if (!(projection.blocks[index - 1].block < projection.blocks[index].block)) {
      return Status::error(ErrorCode::DuplicateIdentity,
                           "topology reserve blocks must be strictly ascending and unique");
    }
  }
  if (state_.has_topology) {
    if (projection.generation < state_.topology.generation) {
      return Status::error(ErrorCode::StaleGeneration, "topology generation is older than current",
                           "requested", projection.generation.value(), "current",
                           state_.topology.generation.value());
    }
    if (projection.generation == state_.topology.generation &&
        state_.topology.stamp.evidence == projection.stamp.evidence) {
      return state_.revision;  // exact replay; idempotent
    }
  }
  CF_TRY(commit(RecordType::TopologyImported, encode_topology(projection)));
  CF_TRY(enforce_bindings(projection.stamp.observed_at));
  return state_.revision;
}

Result<StateRevision> CoolingFailoverOrchestrator::record_capacity(
    CapacityAvailabilityEvidence evidence) {
  CF_TRY(require_open());
  if (!(evidence.domain == domain_)) {
    return Status::error(ErrorCode::InvalidArgument, "capacity evidence belongs to another domain");
  }
  if (evidence.stamp.evidence.is_nil()) {
    return Status::error(ErrorCode::InvalidArgument, "capacity evidence identity is nil");
  }
  if (!evidence.generation.is_set()) {
    return Status::error(ErrorCode::InvalidArgument, "capacity generation must be set");
  }
  if (evidence.blocks.size() > kMaxBlocksPerDomain) {
    return Status::error(ErrorCode::BoundsExceeded, "capacity evidence has too many blocks");
  }
  for (std::size_t index = 1; index < evidence.blocks.size(); ++index) {
    if (!(evidence.blocks[index - 1].block < evidence.blocks[index].block)) {
      return Status::error(ErrorCode::DuplicateIdentity,
                           "capacity evidence blocks must be strictly ascending and unique");
    }
  }
  if (state_.has_topology) {
    for (const BlockAvailability& block : evidence.blocks) {
      bool known = false;
      for (const ReserveBlockDescriptor& descriptor : state_.topology.blocks) {
        if (descriptor.block == block.block) {
          known = true;
          break;
        }
      }
      if (!known) {
        return Status::error(ErrorCode::InvalidArgument,
                             "capacity evidence references an unknown reserve block", "block",
                             block.block.value());
      }
    }
  }
  if (state_.has_capacity && evidence.generation < state_.capacity.generation) {
    return Status::error(ErrorCode::StaleGeneration, "capacity generation is older than current",
                         "requested", evidence.generation.value(), "current",
                         state_.capacity.generation.value());
  }
  CF_TRY(commit(RecordType::CapacityRecorded, encode_capacity(evidence)));
  CF_TRY(enforce_bindings(evidence.stamp.observed_at));
  return state_.revision;
}

Result<StateRevision> CoolingFailoverOrchestrator::record_health(SourceHealthEvidence evidence) {
  CF_TRY(require_open());
  if (evidence.stamp.evidence.is_nil()) {
    return Status::error(ErrorCode::InvalidArgument, "health evidence identity is nil");
  }
  if (evidence.source.is_nil()) {
    return Status::error(ErrorCode::InvalidArgument, "health evidence source identity is nil");
  }
  if (!state_.has_topology) {
    return Status::error(ErrorCode::TopologyNotImported,
                         "health evidence cannot be imported before the topology projection");
  }
  bool known = false;
  for (const SourceDescriptor& source : state_.topology.sources) {
    if (source.source == evidence.source) {
      known = true;
      break;
    }
  }
  if (!known) {
    return Status::error(ErrorCode::InvalidArgument,
                         "health evidence references a source outside the current topology",
                         "source", evidence.source.value());
  }
  const auto existing = state_.health.find(evidence.source);
  if (existing != state_.health.end() &&
      evidence.stamp.observed_at < existing->second.stamp.observed_at) {
    return Status::error(ErrorCode::EvidenceReordered,
                         "health evidence is older than the evidence already recorded", "source",
                         evidence.source.value());
  }
  CF_TRY(commit(RecordType::HealthRecorded, encode_health(evidence)));
  CF_TRY(enforce_bindings(evidence.stamp.observed_at));
  return state_.revision;
}

Result<StateRevision> CoolingFailoverOrchestrator::record_authority(
    ServiceAuthorityEvidence evidence) {
  CF_TRY(require_open());
  if (!(evidence.domain == domain_)) {
    return Status::error(ErrorCode::InvalidArgument, "authority evidence belongs to another domain");
  }
  if (evidence.stamp.evidence.is_nil()) {
    return Status::error(ErrorCode::InvalidArgument, "authority evidence identity is nil");
  }
  if (!evidence.epoch.is_set()) {
    return Status::error(ErrorCode::InvalidArgument, "authority evidence epoch must be set");
  }
  if (state_.epoch_established && evidence.epoch < state_.epoch) {
    return Status::error(ErrorCode::SupersededEpoch,
                         "authority evidence is for a superseded epoch", "authority_epoch",
                         evidence.epoch.value(), "current", state_.epoch.value());
  }
  if (state_.has_authority && evidence.epoch == state_.authority.epoch &&
      evidence.stamp.observed_at < state_.authority.stamp.observed_at) {
    return Status::error(ErrorCode::EvidenceReordered,
                         "authority evidence is older than the evidence already recorded");
  }
  CF_TRY(commit(RecordType::AuthorityRecorded, encode_authority(evidence)));
  CF_TRY(enforce_bindings(evidence.stamp.observed_at));
  return state_.revision;
}

Result<StateRevision> CoolingFailoverOrchestrator::install_policy(RedundancyPolicy policy) {
  CF_TRY(require_open());
  if (policy.stamp.evidence.is_nil()) {
    return Status::error(ErrorCode::InvalidArgument, "policy evidence identity is nil");
  }
  if (!policy.generation.is_set()) {
    return Status::error(ErrorCode::InvalidArgument, "policy generation must be set");
  }
  if (policy.rank_order.size() > kMaxRankCriteria) {
    return Status::error(ErrorCode::BoundsExceeded, "policy declares too many rank criteria");
  }
  if (policy.max_groups_per_candidate == 0 ||
      policy.max_groups_per_candidate > kMaxGroupsPerCandidate) {
    return Status::error(ErrorCode::OutOfRange,
                         "policy max_groups_per_candidate is outside the supported range");
  }
  if (policy.max_plan_steps == 0 || policy.max_plan_steps > kMaxPlanSteps) {
    return Status::error(ErrorCode::OutOfRange,
                         "policy max_plan_steps is outside the supported range");
  }
  if (policy.max_evidence_age_ticks > kMaxEvidenceAgeTicks) {
    return Status::error(ErrorCode::OutOfRange, "policy evidence age bound is too large");
  }
  if (policy.minimum_independent_groups == 0 ||
      policy.minimum_independent_groups > kMaxGroupsPerCandidate) {
    return Status::error(ErrorCode::OutOfRange,
                         "policy minimum_independent_groups is outside the supported range");
  }
  if (policy.selection == SelectionMode::DeterministicRanked && policy.rank_order.empty()) {
    return Status::error(ErrorCode::InvalidArgument,
                         "a deterministic policy must declare at least one rank criterion");
  }
  if (state_.has_policy) {
    if (policy.generation < state_.policy.generation) {
      return Status::error(ErrorCode::StaleGeneration, "policy generation is older than current",
                           "requested", policy.generation.value(), "current",
                           state_.policy.generation.value());
    }
    if (policy.generation == state_.policy.generation &&
        state_.policy.stamp.evidence == policy.stamp.evidence) {
      return state_.revision;  // exact replay; idempotent
    }
  }
  CF_TRY(commit(RecordType::PolicyInstalled, encode_policy(policy)));
  CF_TRY(enforce_bindings(policy.stamp.observed_at));
  return state_.revision;
}

Result<StateRevision> CoolingFailoverOrchestrator::install_obligations(ObligationSet obligations) {
  CF_TRY(require_open());
  if (obligations.stamp.evidence.is_nil()) {
    return Status::error(ErrorCode::InvalidArgument, "obligation evidence identity is nil");
  }
  if (obligations.generation == 0) {
    return Status::error(ErrorCode::InvalidArgument, "obligation generation must be set");
  }
  if (obligations.obligations.size() > kMaxObligationsPerDomain) {
    return Status::error(ErrorCode::BoundsExceeded, "too many protected obligations");
  }
  for (std::size_t index = 0; index < obligations.obligations.size(); ++index) {
    const ProtectedObligation& obligation = obligations.obligations[index];
    if (obligation.id.is_nil()) {
      return Status::error(ErrorCode::InvalidArgument, "obligation identity is nil");
    }
    if (obligation.required_capacity.is_negative()) {
      return Status::error(ErrorCode::InvalidArgument, "obligation requires a negative capacity");
    }
    if (obligation.required_independent_groups == 0 ||
        obligation.required_independent_groups > kMaxGroupsPerCandidate) {
      return Status::error(ErrorCode::OutOfRange,
                           "obligation independence requirement is outside the supported range");
    }
    if (index > 0 && !(obligations.obligations[index - 1].id < obligation.id)) {
      return Status::error(ErrorCode::DuplicateIdentity,
                           "protected obligations must be strictly ascending and unique");
    }
  }
  if (state_.has_obligations && obligations.generation < state_.obligations.generation) {
    return Status::error(ErrorCode::StaleGeneration,
                         "obligation generation is older than current", "requested",
                         obligations.generation, "current", state_.obligations.generation);
  }
  CF_TRY(commit(RecordType::ObligationsInstalled, encode_obligations(obligations)));
  CF_TRY(enforce_bindings(obligations.stamp.observed_at));
  return state_.revision;
}

// ---------------------------------------------------------------------------
// Eligibility
// ---------------------------------------------------------------------------

Result<CandidateSet> CoolingFailoverOrchestrator::evaluate_candidates(Tick now) const {
  CF_TRY(require_open());
  CF_TRY_ASSIGN(arrangements, enumerate_arrangements(state_));
  CandidateSet set;
  set.domain = domain_;
  set.epoch = state_.epoch;
  set.topology = state_.topology.generation;
  set.capacity = state_.capacity.generation;
  set.policy = state_.policy.generation;
  set.evidence_generation = state_.evidence_generation;
  set.evaluated_at = now;
  set.selection = state_.has_policy ? state_.policy.selection : SelectionMode::NotDefined;

  set.candidates.reserve(arrangements.size());
  for (const CandidateKey& key : arrangements) {
    CF_TRY_ASSIGN(candidate, evaluate_arrangement(state_, key, now));
    set.candidates.push_back(std::move(candidate));
  }
  if (state_.has_policy) {
    CF_TRY(rank_candidates(set.candidates, state_.policy));
  }
  CF_TRY_ASSIGN(evidence_digest,
                compute_evidence_digest(state_, std::vector<SourceGroupId>{}, SourceGroupId::nil()));
  set.evidence_digest = evidence_digest;

  if (state_.has_policy && state_.policy.selection == SelectionMode::DeterministicRanked) {
    for (const FailoverCandidate& candidate : set.candidates) {
      if (candidate.rank == 0) {
        set.selected = candidate.id;
        break;
      }
    }
  }
  return set;
}

// ---------------------------------------------------------------------------
// Planning
// ---------------------------------------------------------------------------

Result<FailoverPlan> CoolingFailoverOrchestrator::create_plan(const PlanRequest& request) {
  CF_TRY(require_open());
  if (request.request.is_nil()) {
    return Status::error(ErrorCode::InvalidArgument, "plan request identity is nil");
  }
  if (request.kind < PlanKind::Failover || request.kind > PlanKind::ReturnToPrimary) {
    return Status::error(ErrorCode::InvalidEnumValue, "plan kind is not valid");
  }
  const auto previous = state_.plan_requests.find(request.request);
  if (previous != state_.plan_requests.end()) {
    if (!(previous->second.first == request_fingerprint(request))) {
      return Status::error(ErrorCode::IdempotencyConflict,
                           "plan request identity was used for a different request", "request",
                           request.request.value());
    }
    const auto plan = state_.plans.find(previous->second.second);
    if (plan == state_.plans.end()) {
      return Status::error(ErrorCode::PlanNotFound, "the replayed plan is no longer retained");
    }
    return plan->second;  // replay of the original result
  }
  if (!state_.registered) {
    return Status::error(ErrorCode::InvalidArgument, "domain has not been registered");
  }
  if (!state_.epoch_established) {
    return Status::error(ErrorCode::EpochNotEstablished,
                         "no control-plane epoch has been established");
  }
  if (!state_.has_topology) {
    return Status::error(ErrorCode::TopologyNotImported, "no topology projection is available");
  }
  if (!state_.has_policy) {
    return Status::error(ErrorCode::PolicyNotDefined, "no redundancy policy is installed");
  }
  if (!state_.has_obligations) {
    return Status::error(ErrorCode::EvidenceMissing,
                         "no protected obligations are installed", "cause",
                         to_string(EligibilityCause::ObligationsMissing));
  }
  if (!state_.has_capacity) {
    return Status::error(ErrorCode::EvidenceMissing, "no capacity evidence is current");
  }
  if (request.incumbent.is_nil() || !state_.topology.contains_group(request.incumbent)) {
    return Status::error(ErrorCode::InvalidArgument,
                         "the incumbent source group is not part of the current topology");
  }
  if (!state_.active_plan.is_nil()) {
    return Status::error(ErrorCode::PlanAlreadyActive,
                         "another plan is already active for this protected domain", "plan",
                         state_.active_plan.value());
  }

  CF_TRY_ASSIGN(set, evaluate_candidates(request.now));

  // Resolve the target arrangement.
  CandidateKey key;
  if (!request.target.is_nil()) {
    key.groups.push_back(request.target);
  } else {
    if (request.kind == PlanKind::ReturnToPrimary) {
      SourceGroupId primary = SourceGroupId::nil();
      for (const SourceGroupDescriptor& group : state_.topology.groups) {
        if (group.designated_primary && (primary.is_nil() || group.group < primary)) {
          primary = group.group;
        }
      }
      if (primary.is_nil()) {
        return Status::error(ErrorCode::RecoveryNotEligible,
                             "the topology designates no primary source group");
      }
      key.groups.push_back(primary);
    } else if (state_.policy.selection == SelectionMode::DeterministicRanked) {
      if (set.selected.is_nil()) {
        return Status::error(ErrorCode::NoEligibleCandidate,
                             "no eligible candidate arrangement is available");
      }
      const FailoverCandidate* selected = set.find(set.selected);
      if (selected == nullptr) {
        return Status::error(ErrorCode::InternalError, "selected candidate is missing from the set");
      }
      key = selected->key;
    } else {
      return Status::error(ErrorCode::SelectionNotDefined,
                           "the policy does not define a deterministic selection and no target was "
                           "named");
    }
  }

  if (request.kind == PlanKind::Failover && key.groups.size() == 1 &&
      key.groups[0] == request.incumbent) {
    return Status::error(
        ErrorCode::InvalidArgument,
        "a failover plan cannot target the incumbent arrangement as its own alternate");
  }

  const FailoverCandidate* candidate = set.find(key);
  if (candidate == nullptr) {
    return Status::error(ErrorCode::InvalidArgument,
                         "the requested target is not a candidate arrangement for this topology");
  }
  if (!candidate->is_eligible()) {
    Status status = Status::error(ErrorCode::NoEligibleCandidate,
                                  "the requested target arrangement is not eligible");
    if (!candidate->causes.empty()) {
      status.with("cause", to_string(candidate->causes.front()));
      status.with("cause_count", static_cast<std::uint64_t>(candidate->causes.size()));
    }
    return status;
  }

  CF_TRY_ASSIGN(evidence_digest,
                compute_evidence_digest(state_, key.groups, request.incumbent));

  FailoverPlan plan;
  plan.domain = domain_;
  plan.kind = request.kind;
  plan.epoch = state_.epoch;
  plan.revision_at_creation = state_.revision;
  plan.topology = state_.topology.generation;
  plan.capacity = state_.capacity.generation;
  plan.policy = state_.policy.generation;
  plan.evidence_generation = state_.evidence_generation;
  plan.evidence_digest = evidence_digest;
  plan.obligations_generation = state_.obligations.generation;
  plan.incumbent = request.incumbent;
  plan.target = key.groups.front();
  plan.target_groups = key.groups;
  plan.created_at = request.now;
  plan.state = PlanState::Active;
  plan.reason = request.reason;
  {
    const Digest fingerprint = plan_fingerprint(request, key, state_.epoch, request.now);
    plan.id = PlanId::from_value(digest_word(fingerprint, 0));
  }

  // Deterministic step and command construction.
  std::uint32_t ordinal = 0;
  for (std::size_t index = 0; index < key.groups.size(); ++index) {
    PlanStep step;
    step.ordinal = ordinal++;
    step.target = key.groups[index];
    step.required_effects = effects_for_group(state_, index, key.groups.size());
    plan.steps.push_back(std::move(step));
  }
  if (request.kind == PlanKind::Failover &&
      std::find(key.groups.begin(), key.groups.end(), request.incumbent) == key.groups.end()) {
    PlanStep step;
    step.ordinal = ordinal++;
    step.target = request.incumbent;
    step.required_effects.push_back(EffectKind::IncumbentIsolated);
    plan.steps.push_back(std::move(step));
  }
  if (plan.steps.size() > state_.policy.max_plan_steps) {
    return Status::error(ErrorCode::LimitExceeded, "the plan exceeds the policy step bound",
                         "steps", static_cast<std::uint64_t>(plan.steps.size()), "bound",
                         static_cast<std::uint64_t>(state_.policy.max_plan_steps));
  }

  for (const PlanStep& step : plan.steps) {
    if (!state_.topology.contains_group(step.target)) {
      return Status::error(ErrorCode::InvalidArgument,
                           "plan step references a group outside the topology");
    }
    for (OwnerSystem runtime : runtimes_of_group(state_.topology, step.target)) {
      CommandSpec command;
      command.step = step.ordinal;
      command.runtime = runtime;
      command.target = step.target;
      command.required_effects = step.required_effects;
      command.key = derive_key(plan.id, step.ordinal, runtime);
      command.id = derive_command_id(plan.id, step.ordinal, runtime);
      command.fingerprint = command_fingerprint(plan.id, step.ordinal, runtime, step.target,
                                                step.required_effects);
      plan.commands.push_back(std::move(command));
    }
  }
  std::stable_sort(plan.commands.begin(), plan.commands.end(),
                   [](const CommandSpec& lhs, const CommandSpec& rhs) {
                     if (lhs.step != rhs.step) {
                       return lhs.step < rhs.step;
                     }
                     return static_cast<std::uint8_t>(lhs.runtime) <
                            static_cast<std::uint8_t>(rhs.runtime);
                   });

  plan.request = request.request;
  plan.request_fingerprint = request_fingerprint(request);
  CF_TRY(commit(RecordType::PlanCreated, encode_plan(plan)));
  return plan;
}

Result<FailoverPlan> CoolingFailoverOrchestrator::get_plan(PlanId plan) const {
  const auto it = state_.plans.find(plan);
  if (it == state_.plans.end()) {
    return Status::error(ErrorCode::PlanNotFound, "plan is unknown", "plan", plan.value());
  }
  return it->second;
}

// ---------------------------------------------------------------------------
// Transition
// ---------------------------------------------------------------------------

VerificationSummary CoolingFailoverOrchestrator::recompute_verification(
    const FailoverPlan& plan, TransitionAttempt& attempt) const {
  VerificationSummary summary;
  std::vector<EffectKind> required;
  for (const CommandSpec& command : plan.commands) {
    for (EffectKind effect : command.required_effects) {
      required.push_back(effect);
    }
  }
  std::sort(required.begin(), required.end());
  required.erase(std::unique(required.begin(), required.end()), required.end());

  for (EffectKind effect : required) {
    bool satisfied = true;
    for (const CommandSpec& command : plan.commands) {
      if (!contains_effect(command.required_effects, effect)) {
        continue;
      }
      const auto record = state_.commands.find(command.id);
      if (record == state_.commands.end()) {
        satisfied = false;
        break;
      }
      bool observed = false;
      for (const auto& entry : state_.observations) {
        const EffectObservationRecord& observation = entry.second;
        if (!(observation.command == command.id) || !(observation.group == command.target) ||
            observation.effect != effect) {
          continue;
        }
        if (!origin_can_verify(observation.origin) || observation.recovered_unvalidated) {
          continue;
        }
        if (!(observation.epoch == plan.epoch) || !(observation.topology == plan.topology) ||
            !(observation.capacity == plan.capacity)) {
          continue;
        }
        if (observation.sequence.value() <= record->second.issue_sequence.value()) {
          continue;
        }
        if (observation.observed_at < record->second.issued_at) {
          continue;
        }
        observed = true;
        break;
      }
      if (!observed) {
        satisfied = false;
        break;
      }
    }
    if (satisfied) {
      summary.verified.push_back(effect);
    } else {
      summary.missing.push_back(effect);
    }
  }

  std::uint32_t issued = 0;
  std::uint32_t acknowledged = 0;
  std::uint32_t refused = 0;
  for (const CommandSpec& command : plan.commands) {
    const auto record = state_.commands.find(command.id);
    if (record == state_.commands.end()) {
      ++refused;
      continue;
    }
    ++issued;
    if (command_outcome_counts_as_acknowledged(record->second.outcome)) {
      ++acknowledged;
    } else if (command_outcome_counts_as_refused(record->second.outcome)) {
      ++refused;
    }
  }
  const std::size_t total = plan.commands.size();
  if (issued == 0) {
    summary.state = AttemptState::Created;
  } else if (summary.missing.empty() && acknowledged == total) {
    summary.state = AttemptState::Verified;
  } else if (!summary.verified.empty()) {
    summary.state = AttemptState::PartiallyObserved;
  } else if (acknowledged == 0 && refused == total) {
    summary.state = AttemptState::Failed;
  } else if (acknowledged > 0) {
    summary.state = AttemptState::Acknowledged;
  } else {
    summary.state = AttemptState::RequestsIssued;
  }
  summary.complete = summary.state == AttemptState::Verified;

  attempt.verified_effects = summary.verified;
  attempt.missing_effects = summary.missing;
  attempt.commands_issued = issued;
  attempt.commands_refused = refused;
  return summary;
}

Result<TransitionAttempt> CoolingFailoverOrchestrator::execute_plan(PlanId plan_id, Tick now) {
  CF_TRY(require_open());
  const auto plan_it = state_.plans.find(plan_id);
  if (plan_it == state_.plans.end()) {
    return Status::error(ErrorCode::PlanNotFound, "plan is unknown", "plan", plan_id.value());
  }
  FailoverPlan plan = plan_it->second;

  Result<void> live = assert_plan_live(plan);
  if (!live.has_value()) {
    const Status failure = live.status();
    if (plan.state == PlanState::Active) {
      CF_TRY(fence_plan_internal(plan_id, fence_reason_for(failure), now));
    }
    return failure;
  }

  // Reuse the in-flight attempt when there is one; otherwise open a new attempt.
  AttemptId attempt_id = AttemptId::nil();
  if (state_.active_attempt.is_nil()) {
    for (const auto& entry : state_.attempts) {
      if ((entry.second.plan == plan_id) && !attempt_state_is_terminal(entry.second.state)) {
        attempt_id = entry.first;
        break;
      }
    }
  } else {
    const auto existing = state_.attempts.find(state_.active_attempt);
    if (existing != state_.attempts.end() && (existing->second.plan == plan_id)) {
      attempt_id = state_.active_attempt;
    }
  }

  if (attempt_id.is_nil()) {
    TransitionAttempt attempt;
    attempt.number = plan.attempts_started + 1;
    attempt.id = derive_attempt_id(plan.id, attempt.number);
    attempt.plan = plan.id;
    attempt.epoch = plan.epoch;
    attempt.state = AttemptState::Created;
    attempt.created_at = now;
    attempt.updated_at = now;
    attempt.revision_at_issue = state_.revision;
    attempt.plan_binding = plan.binding_digest();
    for (const CommandSpec& command : plan.commands) {
      attempt.commands.push_back(command.id);
    }
    plan.attempts_started = attempt.number;
    const AttemptId opened = attempt.id;
    CF_TRY(commit(RecordType::PlanCreated, encode_plan(plan)));
    CF_TRY(update_attempt(std::move(attempt)));
    attempt_id = opened;
  }

  TransitionAttempt attempt = state_.attempts.at(attempt_id);
  IssueSequence issue_sequence = IssueSequence::initial();
  for (const auto& entry : state_.commands) {
    if (entry.second.issue_sequence > issue_sequence) {
      issue_sequence = entry.second.issue_sequence;
    }
  }

  std::uint32_t issued_this_call = 0;
  std::uint32_t rejected_observations = 0;
  const std::uint32_t budget =
      std::min(options_.max_commands_per_execution == 0 ? kMaxPlanSteps * 4U
                                                        : options_.max_commands_per_execution,
               kMaxPlanSteps * 4U);

  for (const CommandSpec& command : plan.commands) {
    if (issued_this_call >= budget) {
      break;
    }
    const auto recorded = state_.commands.find(command.id);
    if (recorded != state_.commands.end()) {
      // Already issued durably: never re-issued. Poll the owning runtime again so
      // that a restarted orchestrator can still complete verification.
      if (options_.pull_effects_after_issue &&
          recorded->second.outcome == CommandOutcome::Acknowledged) {
        ControlRuntimePort* pending = runtimes_.find(command.runtime);
        if (pending != nullptr) {
          ControlRequest poll_request;
          poll_request.command = command.id;
          poll_request.key = command.key;
          poll_request.fingerprint = command.fingerprint;
          poll_request.domain = domain_;
          poll_request.plan = plan.id;
          poll_request.step = command.step;
          poll_request.runtime = command.runtime;
          poll_request.target = command.target;
          poll_request.requested_effects = command.required_effects;
          poll_request.epoch = plan.epoch;
          poll_request.issued_at = recorded->second.issued_at;
          Result<std::vector<EffectObservationRecord>> pulled = pending->poll_effects(poll_request);
          if (pulled.has_value()) {
            for (const EffectObservationRecord& observation : *pulled) {
              Result<TransitionAttempt> accepted = submit_observation(observation);
              if (!accepted.has_value()) {
                ++rejected_observations;
              }
            }
          }
        }
      }
      continue;
    }
    const auto key_it = state_.idempotency.find(command.key);
    if (key_it != state_.idempotency.end()) {
      if (!(key_it->second.fingerprint == command.fingerprint)) {
        return Status::error(ErrorCode::IdempotencyConflict,
                             "an idempotency key was reused for a different command", "command",
                             command.id.value());
      }
      CommandRecord replay;
      replay.id = command.id;
      replay.key = command.key;
      replay.fingerprint = command.fingerprint;
      replay.plan = plan.id;
      replay.attempt = attempt_id;
      replay.step = command.step;
      replay.runtime = command.runtime;
      replay.target = command.target;
      replay.outcome = key_it->second.outcome;
      replay.issue_sequence = issue_sequence;
      replay.issued_at = now;
      replay.epoch = plan.epoch;
      replay.detail = "replayed from the durable idempotency record";
      CF_TRY(commit(RecordType::CommandRecorded, encode_command(replay)));
      ++issued_this_call;
      continue;
    }

    Result<void> still_live = assert_plan_live(plan);
    if (!still_live.has_value()) {
      const Status failure = still_live.status();
      CF_TRY(fence_plan_internal(plan_id, fence_reason_for(failure), now));
      attempt = state_.attempts.at(attempt_id);
      attempt.state = AttemptState::Fenced;
      attempt.updated_at = now;
      attempt.detail = std::string("plan fenced before command issue: ") + to_string(failure.code());
      CF_TRY(update_attempt(std::move(attempt)));
      return failure;
    }

    CF_TRY_ASSIGN(next_sequence, issue_sequence.next());
    issue_sequence = next_sequence;

    ControlRequest request;
    request.command = command.id;
    request.key = command.key;
    request.fingerprint = command.fingerprint;
    request.domain = domain_;
    request.plan = plan.id;
    request.step = command.step;
    request.runtime = command.runtime;
    request.target = command.target;
    request.requested_effects = command.required_effects;
    request.epoch = plan.epoch;
    request.issued_at = now;

    CommandRecord record;
    record.id = command.id;
    record.key = command.key;
    record.fingerprint = command.fingerprint;
    record.plan = plan.id;
    record.attempt = attempt_id;
    record.step = command.step;
    record.runtime = command.runtime;
    record.target = command.target;
    record.issue_sequence = issue_sequence;
    record.issued_at = now;
    record.epoch = plan.epoch;

    ControlRuntimePort* port = runtimes_.find(command.runtime);
    if (port == nullptr) {
      record.outcome = CommandOutcome::RuntimeUnavailable;
      record.detail = "no control runtime port is registered for this runtime";
    } else {
      Result<ControlResponse> response = port->issue(request);
      if (!response.has_value()) {
        record.outcome = response.status().code() == ErrorCode::AdapterUnavailable
                             ? CommandOutcome::RuntimeUnavailable
                             : CommandOutcome::Rejected;
        record.detail = response.status().canonical_line();
      } else {
        record.outcome = response->outcome;
        record.detail = response->detail;
      }
    }
    // The issue is recorded durably before any of its observations are accepted,
    // so an observation can never reference a command that was not journaled.
    CF_TRY(commit(RecordType::CommandRecorded, encode_command(record)));
    ++issued_this_call;

    if (port != nullptr && record.outcome == CommandOutcome::Acknowledged &&
        options_.pull_effects_after_issue) {
      Result<std::vector<EffectObservationRecord>> pulled = port->poll_effects(request);
      if (pulled.has_value()) {
        for (const EffectObservationRecord& observation : *pulled) {
          Result<TransitionAttempt> accepted = submit_observation(observation);
          if (!accepted.has_value()) {
            ++rejected_observations;
          }
        }
      }
    }
  }

  CF_TRY_ASSIGN(current, get_attempt(attempt_id));
  TransitionAttempt updated = current;
  updated.updated_at = now;
  updated.commands_replayed = 0;
  for (const CommandSpec& command : plan.commands) {
    const auto record = state_.commands.find(command.id);
    if (record != state_.commands.end() && record->second.outcome == CommandOutcome::Replayed) {
      ++updated.commands_replayed;
    }
  }
  {
    std::uint32_t accepted = 0;
    for (const auto& entry : state_.observations) {
      for (const CommandSpec& command : plan.commands) {
        if (entry.second.command == command.id) {
          ++accepted;
          break;
        }
      }
    }
    updated.observations_accepted = accepted;
  }
  const VerificationSummary summary = recompute_verification(plan, updated);
  updated.state = summary.state;
  if (summary.complete) {
    updated.detail = "every required effect observed";
  } else if (rejected_observations > 0) {
    updated.detail = "waiting for observed cooling effects; " +
                     std::to_string(rejected_observations) + " observation(s) rejected";
  } else {
    updated.detail = "waiting for observed cooling effects";
  }
  CF_TRY(update_attempt(std::move(updated)));

  if (summary.complete) {
    CF_TRY(commit(RecordType::PlanStateChanged,
                  encode_plan_state(plan.id, PlanState::Completed, FenceReason::None, now)));
    TransitionRecord transition;
    transition.plan = plan.id;
    transition.kind = plan.kind;
    transition.from_group = plan.incumbent;
    transition.to_group = plan.target;
    transition.at = now;
    CF_TRY(commit(RecordType::TransitionRecorded, encode_transition(transition)));
  }
  return get_attempt(attempt_id);
}

Result<TransitionAttempt> CoolingFailoverOrchestrator::get_attempt(AttemptId attempt) const {
  const auto it = state_.attempts.find(attempt);
  if (it == state_.attempts.end()) {
    return Status::error(ErrorCode::AttemptNotFound, "attempt is unknown", "attempt",
                         attempt.value());
  }
  return it->second;
}

Result<VerificationSummary> CoolingFailoverOrchestrator::summarize_attempt(AttemptId attempt) const {
  CF_TRY_ASSIGN(found, get_attempt(attempt));
  const auto plan = state_.plans.find(found.plan);
  if (plan == state_.plans.end()) {
    return Status::error(ErrorCode::PlanNotFound, "the attempt's plan is not retained");
  }
  TransitionAttempt copy = found;
  return recompute_verification(plan->second, copy);
}

Result<TransitionAttempt> CoolingFailoverOrchestrator::verify_attempt(AttemptId attempt) {
  CF_TRY(require_open());
  CF_TRY_ASSIGN(found, get_attempt(attempt));
  if (attempt_state_is_terminal(found.state)) {
    return found;
  }
  const auto plan = state_.plans.find(found.plan);
  if (plan == state_.plans.end()) {
    return Status::error(ErrorCode::PlanNotFound, "the attempt's plan is not retained");
  }
  TransitionAttempt updated = found;
  updated.updated_at = state_.last_tick;
  const VerificationSummary summary = recompute_verification(plan->second, updated);
  updated.state = summary.state;
  updated.detail = summary.complete ? "every required effect observed"
                                    : "waiting for observed cooling effects";
  CF_TRY(update_attempt(std::move(updated)));
  if (summary.complete && plan->second.state == PlanState::Active) {
    CF_TRY(commit(RecordType::PlanStateChanged,
                  encode_plan_state(plan->first, PlanState::Completed, FenceReason::None,
                                    state_.last_tick)));
    TransitionRecord transition;
    transition.plan = plan->first;
    transition.kind = plan->second.kind;
    transition.from_group = plan->second.incumbent;
    transition.to_group = plan->second.target;
    transition.at = state_.last_tick;
    CF_TRY(commit(RecordType::TransitionRecorded, encode_transition(transition)));
  }
  return get_attempt(attempt);
}

Result<TransitionAttempt> CoolingFailoverOrchestrator::submit_observation(
    EffectObservationRecord observation) {
  CF_TRY(require_open());
  if (observation.id.is_nil()) {
    return Status::error(ErrorCode::InvalidArgument, "observation identity is nil");
  }
  if (observation.command.is_nil()) {
    return Status::error(ErrorCode::InvalidArgument, "observation command identity is nil");
  }
  if (observation.origin < ObservationOrigin::AirflowControl ||
      observation.origin > ObservationOrigin::ActuationControllerAck) {
    return Status::error(ErrorCode::InvalidEnumValue, "observation origin is not valid");
  }
  if (observation.effect < EffectKind::FlowEstablished ||
      observation.effect > EffectKind::RedundancyRestored) {
    return Status::error(ErrorCode::InvalidEnumValue, "observation effect is not valid");
  }
  const auto existing = state_.observations.find(observation.id);
  if (existing != state_.observations.end()) {
    const EffectObservationRecord& previous = existing->second;
    const bool identical = (previous.command == observation.command) &&
                           (previous.group == observation.group) &&
                           (previous.effect == observation.effect) &&
                           (previous.origin == observation.origin) &&
                           (previous.sequence == observation.sequence) &&
                           (previous.observed_at == observation.observed_at);
    if (!identical) {
      return Status::error(ErrorCode::DuplicateIdentity,
                           "an observation identity was reused for different evidence", "observation",
                           observation.id.value());
    }
    // Exact duplicate: accepted as a replay, nothing changes.
    const auto command = state_.commands.find(observation.command);
    if (command == state_.commands.end()) {
      return Status::error(ErrorCode::ObservationUnknownCommand,
                           "observation references a command that was never issued", "command",
                           observation.command.value());
    }
    return get_attempt(command->second.attempt);
  }

  const auto command = state_.commands.find(observation.command);
  if (command == state_.commands.end()) {
    return Status::error(ErrorCode::ObservationUnknownCommand,
                         "observation references a command that was never issued", "command",
                         observation.command.value());
  }
  if (observation.observed_at < command->second.issued_at) {
    return Status::error(ErrorCode::EvidenceReordered,
                         "observation is dated before the command it reports on");
  }
  for (const auto& entry : state_.observations) {
    const EffectObservationRecord& previous = entry.second;
    if (!(previous.command == observation.command) || previous.effect != observation.effect ||
        previous.origin != observation.origin) {
      continue;
    }
    if (observation.sequence.value() <= previous.sequence.value()) {
      return Status::error(ErrorCode::EvidenceReordered,
                           "observation sequence is not monotonic for this effect stream");
    }
  }

  EffectObservationRecord stored = observation;
  stored.recovered_unvalidated = false;
  CF_TRY(commit(RecordType::ObservationRecorded, encode_observation(stored)));
  return get_attempt(command->second.attempt);
}

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------

Result<RecoveryDecision> CoolingFailoverOrchestrator::evaluate_recovery(Tick now) {
  CF_TRY(require_open());
  RecoveryDecision decision;
  if (!state_.has_topology) {
    decision.primary_cause = EligibilityCause::TopologyNotImported;
    decision.detail = "no topology projection is available";
    return decision;
  }
  if (!state_.has_policy) {
    decision.primary_cause = EligibilityCause::PolicyMissing;
    decision.detail = "no redundancy policy is installed";
    return decision;
  }

  // The arrangement currently in service: the target of the most recent
  // completed plan, otherwise the designated primary.
  SourceGroupId incumbent = SourceGroupId::nil();
  for (const auto& entry : state_.plans) {
    if (entry.second.state == PlanState::Completed &&
        (incumbent.is_nil() || entry.second.target < incumbent)) {
      incumbent = entry.second.target;
    }
  }
  if (incumbent.is_nil()) {
    for (const SourceGroupDescriptor& group : state_.topology.groups) {
      if (group.designated_primary && (incumbent.is_nil() || group.group < incumbent)) {
        incumbent = group.group;
      }
    }
  }
  decision.incumbent = incumbent;

  SourceGroupId primary = SourceGroupId::nil();
  for (const SourceGroupDescriptor& group : state_.topology.groups) {
    if (group.designated_primary && (primary.is_nil() || group.group < primary)) {
      primary = group.group;
    }
  }
  if (primary.is_nil()) {
    decision.primary_cause = EligibilityCause::GroupNotInTopology;
    decision.detail = "the topology designates no primary source group";
    return decision;
  }
  if (primary == incumbent) {
    decision.primary_cause = EligibilityCause::None;
    decision.detail = "service already rests on the primary arrangement";
    return decision;
  }

  decision.kind = PlanKind::ReturnToPrimary;
  decision.target = primary;

  const auto streak = state_.streaks.find(primary);
  if (streak == state_.streaks.end()) {
    decision.primary_cause = EligibilityCause::HealthMissing;
    decision.detail = "no current health evidence has been recorded for the primary group";
    return decision;
  }
  decision.consecutive_healthy = streak->second.consecutive_healthy;
  decision.dwell_ticks = now.elapsed_since(streak->second.streak_started_at);
  if (streak->second.consecutive_healthy < state_.policy.recovery_consecutive_healthy) {
    decision.primary_cause = EligibilityCause::HealthMissing;
    decision.detail = "the primary group has not been healthy for long enough";
    return decision;
  }
  if (decision.dwell_ticks < state_.policy.recovery_dwell_ticks) {
    decision.primary_cause = EligibilityCause::HealthMissing;
    decision.detail = "the primary health streak has not dwelled long enough";
    return decision;
  }

  std::uint32_t transitions = 0;
  for (const TransitionRecord& record : state_.transitions) {
    if (now.elapsed_since(record.at) <= state_.policy.oscillation_window_ticks) {
      ++transitions;
    }
  }
  decision.transitions_in_window = transitions;
  if (state_.policy.max_transitions_per_window != 0 &&
      transitions >= state_.policy.max_transitions_per_window) {
    decision.primary_cause = EligibilityCause::None;
    decision.detail = "the oscillation guard refuses another transition in this window";
    return decision;
  }

  CF_TRY_ASSIGN(set, evaluate_candidates(now));
  CandidateKey key;
  key.groups.push_back(primary);
  const FailoverCandidate* candidate = set.find(key);
  if (candidate == nullptr || !candidate->is_eligible()) {
    decision.primary_cause = candidate == nullptr || candidate->causes.empty()
                                 ? EligibilityCause::NoUsableReserve
                                 : candidate->causes.front();
    decision.detail = "the primary arrangement is not currently eligible";
    return decision;
  }
  decision.selected = candidate->id;
  decision.eligible = true;
  decision.detail = "the primary arrangement is healthy, eligibility is current, and the "
                    "oscillation guard permits the transition";
  return decision;
}

Result<void> CoolingFailoverOrchestrator::checkpoint() {
  CF_TRY(require_open());
  const std::vector<std::uint8_t> snapshot = encode_snapshot_events(state_);
  CF_TRY(store_->checkpoint(snapshot));
  return Result<void>();
}

// ---------------------------------------------------------------------------
// Inspection
// ---------------------------------------------------------------------------

Result<DomainStatus> CoolingFailoverOrchestrator::status() const {
  CF_TRY(require_open());
  DomainStatus snapshot;
  snapshot.domain = domain_;
  snapshot.revision = state_.revision;
  snapshot.commit = state_.last_commit;
  snapshot.epoch = state_.epoch;
  snapshot.epoch_established = state_.epoch_established;
  snapshot.recovered = state_.recovered;
  snapshot.authority = state_.has_authority ? state_.authority.state : AuthorityState::Unknown;
  snapshot.topology = state_.topology.generation;
  snapshot.capacity = state_.capacity.generation;
  snapshot.policy = state_.policy.generation;
  snapshot.has_topology = state_.has_topology;
  snapshot.has_capacity = state_.has_capacity;
  snapshot.has_policy = state_.has_policy;
  snapshot.has_obligations = state_.has_obligations;
  snapshot.group_count = state_.topology.groups.size();
  snapshot.source_count = state_.topology.sources.size();
  snapshot.block_count = state_.topology.blocks.size();
  snapshot.plan_count = state_.plans.size();
  snapshot.attempt_count = state_.attempts.size();
  snapshot.observation_count = state_.observations.size();
  snapshot.active_plan = state_.active_plan;
  snapshot.active_attempt = state_.active_attempt;
  if (!state_.active_attempt.is_nil()) {
    const auto it = state_.attempts.find(state_.active_attempt);
    if (it != state_.attempts.end()) {
      snapshot.active_attempt_state = it->second.state;
      snapshot.degraded = it->second.state == AttemptState::PartiallyObserved ||
                          it->second.state == AttemptState::Interrupted;
    }
  }
  snapshot.state_digest = state_digest();
  return snapshot;
}

std::vector<std::uint8_t> CoolingFailoverOrchestrator::canonical_state() const {
  return encode_state_canonical(state_);
}

Digest CoolingFailoverOrchestrator::state_digest() const {
  const std::vector<std::uint8_t> bytes = canonical_state();
  return sha256(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
}

}  // namespace cooling_failover
