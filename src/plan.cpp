#include "cooling_failover/plan.hpp"

namespace cooling_failover {

const char* to_string(EffectKind effect) noexcept {
  switch (effect) {
    case EffectKind::FlowEstablished: return "FlowEstablished";
    case EffectKind::CapacityDelivered: return "CapacityDelivered";
    case EffectKind::ReturnTemperatureInBand: return "ReturnTemperatureInBand";
    case EffectKind::IncumbentIsolated: return "IncumbentIsolated";
    case EffectKind::RedundancyRestored: return "RedundancyRestored";
  }
  return "Invalid";
}

const char* to_string(PlanKind kind) noexcept {
  switch (kind) {
    case PlanKind::Failover: return "Failover";
    case PlanKind::Rebalance: return "Rebalance";
    case PlanKind::ReturnToPrimary: return "ReturnToPrimary";
  }
  return "Invalid";
}

const char* to_string(ObservationOrigin origin) noexcept {
  switch (origin) {
    case ObservationOrigin::AirflowControl: return "AirflowControl";
    case ObservationOrigin::LiquidCoolingControl: return "LiquidCoolingControl";
    case ObservationOrigin::CoolingCapacity: return "CoolingCapacity";
    case ObservationOrigin::CoolingTopology: return "CoolingTopology";
    case ObservationOrigin::FacilityBms: return "FacilityBms";
    case ObservationOrigin::ActuationControllerAck: return "ActuationControllerAck";
  }
  return "Invalid";
}

bool origin_can_verify(ObservationOrigin origin) noexcept {
  return origin != ObservationOrigin::ActuationControllerAck;
}

const char* to_string(PlanState state) noexcept {
  switch (state) {
    case PlanState::Active: return "Active";
    case PlanState::Completed: return "Completed";
    case PlanState::Fenced: return "Fenced";
    case PlanState::Failed: return "Failed";
    case PlanState::Superseded: return "Superseded";
  }
  return "Invalid";
}

const char* to_string(FenceReason reason) noexcept {
  switch (reason) {
    case FenceReason::None: return "None";
    case FenceReason::EpochSuperseded: return "EpochSuperseded";
    case FenceReason::TopologyGenerationChanged: return "TopologyGenerationChanged";
    case FenceReason::CapacityGenerationChanged: return "CapacityGenerationChanged";
    case FenceReason::PolicyGenerationChanged: return "PolicyGenerationChanged";
    case FenceReason::EvidenceChanged: return "EvidenceChanged";
    case FenceReason::AuthorityLost: return "AuthorityLost";
    case FenceReason::Manual: return "Manual";
    case FenceReason::RequestedByCaller: return "RequestedByCaller";
  }
  return "Invalid";
}

const char* to_string(AttemptState state) noexcept {
  switch (state) {
    case AttemptState::Created: return "Created";
    case AttemptState::RequestsIssued: return "RequestsIssued";
    case AttemptState::Acknowledged: return "Acknowledged";
    case AttemptState::PartiallyObserved: return "PartiallyObserved";
    case AttemptState::Verified: return "Verified";
    case AttemptState::Failed: return "Failed";
    case AttemptState::Fenced: return "Fenced";
    case AttemptState::Interrupted: return "Interrupted";
    case AttemptState::Refused: return "Refused";
  }
  return "Invalid";
}

bool attempt_state_is_terminal(AttemptState state) noexcept {
  switch (state) {
    case AttemptState::Verified:
    case AttemptState::Failed:
    case AttemptState::Fenced:
    case AttemptState::Refused:
      return true;
    case AttemptState::Created:
    case AttemptState::RequestsIssued:
    case AttemptState::Acknowledged:
    case AttemptState::PartiallyObserved:
    case AttemptState::Interrupted:
      return false;
  }
  return false;
}

const char* to_string(CommandOutcome outcome) noexcept {
  switch (outcome) {
    case CommandOutcome::NotIssued: return "NotIssued";
    case CommandOutcome::Acknowledged: return "Acknowledged";
    case CommandOutcome::Refused: return "Refused";
    case CommandOutcome::RuntimeUnavailable: return "RuntimeUnavailable";
    case CommandOutcome::Replayed: return "Replayed";
    case CommandOutcome::Rejected: return "Rejected";
  }
  return "Invalid";
}

Digest FailoverPlan::binding_digest() const {
  Encoder encoder;
  encoder.id(id);
  encoder.id(domain);
  encoder.u8(static_cast<std::uint8_t>(kind));
  encoder.generation(epoch);
  encoder.counter(revision_at_creation);
  encoder.generation(topology);
  encoder.generation(capacity);
  encoder.generation(policy);
  encoder.generation(evidence_generation);
  encoder.digest(evidence_digest);
  encoder.u64(obligations_generation);
  encoder.id(incumbent);
  encoder.id(target);
  encoder.collection(target_groups);
  for (SourceGroupId group : target_groups) {
    encoder.id(group);
  }
  encoder.collection(steps);
  for (const PlanStep& step : steps) {
    encoder.u32(step.ordinal);
    encoder.id(step.target);
    encoder.collection(step.required_effects);
    for (EffectKind effect : step.required_effects) {
      encoder.u8(static_cast<std::uint8_t>(effect));
    }
  }
  encoder.collection(commands);
  for (const CommandSpec& command : commands) {
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
  return encoder.finish_digest();
}

}  // namespace cooling_failover
