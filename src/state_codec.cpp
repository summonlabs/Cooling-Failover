#include "cooling_failover/state.hpp"

#include <algorithm>

namespace cooling_failover {
namespace {

void append(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> bytes) {
  out.insert(out.end(), bytes.begin(), bytes.end());
}

void emit(std::vector<std::uint8_t>& out, RecordType type, const std::vector<std::uint8_t>& payload) {
  Encoder header;
  header.u32(static_cast<std::uint32_t>(payload.size()));
  header.u16(static_cast<std::uint16_t>(type));
  append(out, header.data());
  append(out, payload);
}

}  // namespace

std::vector<std::uint8_t> encode_snapshot_events(const DomainState& state) {
  std::vector<std::uint8_t> out;
  if (state.registered) {
    emit(out, RecordType::DomainRegistered, encode_domain_registered(state.registered_at));
  }
  if (state.epoch_established) {
    emit(out, RecordType::EpochEstablished, encode_epoch_established(state.epoch, state.last_tick));
  }
  if (state.has_topology) {
    emit(out, RecordType::TopologyImported, encode_topology(state.topology));
  }
  if (state.has_capacity) {
    emit(out, RecordType::CapacityRecorded, encode_capacity(state.capacity));
  }
  if (state.has_policy) {
    emit(out, RecordType::PolicyInstalled, encode_policy(state.policy));
  }
  if (state.has_obligations) {
    emit(out, RecordType::ObligationsInstalled, encode_obligations(state.obligations));
  }
  if (state.has_authority) {
    emit(out, RecordType::AuthorityRecorded, encode_authority(state.authority));
  }
  for (const auto& entry : state.health) {
    emit(out, RecordType::HealthRecorded, encode_health(entry.second));
  }
  for (const auto& entry : state.plans) {
    emit(out, RecordType::PlanCreated, encode_plan(entry.second));
  }
  for (const auto& entry : state.plans) {
    if (entry.second.state != PlanState::Active) {
      emit(out, RecordType::PlanStateChanged,
           encode_plan_state(entry.first, entry.second.state, entry.second.fence_reason,
                             entry.second.fenced_at));
    }
  }
  for (const auto& entry : state.attempts) {
    emit(out, RecordType::AttemptRecorded, encode_attempt(entry.second));
  }
  for (const auto& entry : state.commands) {
    emit(out, RecordType::CommandRecorded, encode_command(entry.second));
  }
  for (ObservationId id : state.observation_order) {
    const auto it = state.observations.find(id);
    if (it != state.observations.end()) {
      emit(out, RecordType::ObservationRecorded, encode_observation(it->second));
    }
  }
  for (const TransitionRecord& record : state.transitions) {
    emit(out, RecordType::TransitionRecorded, encode_transition(record));
  }
  return out;
}

Result<std::vector<std::pair<RecordType, std::vector<std::uint8_t>>>> decode_event_stream(
    std::span<const std::uint8_t> bytes) {
  std::vector<std::pair<RecordType, std::vector<std::uint8_t>>> events;
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    if (bytes.size() - offset < 6) {
      return Status::error(ErrorCode::StoreTruncated, "event stream header is incomplete");
    }
    const std::uint32_t length = static_cast<std::uint32_t>(bytes[offset]) |
                                 (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
                                 (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
                                 (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
    const std::uint16_t raw_type = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[offset + 4]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 5]) << 8));
    offset += 6;
    if (length > kMaxBlobBytes) {
      return Status::error(ErrorCode::BoundsExceeded,
                           "event stream entry declares an impossible length");
    }
    if (bytes.size() - offset < length) {
      return Status::error(ErrorCode::StoreTruncated, "event stream entry is incomplete");
    }
    std::vector<std::uint8_t> payload(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                      bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
    offset += length;
    events.emplace_back(static_cast<RecordType>(raw_type), std::move(payload));
    if (events.size() > 4 * (kMaxRetainedPlans + kMaxRetainedAttempts + kMaxRetainedCommands +
                             kMaxRetainedObservations + kMaxTransitionHistory + 64)) {
      return Status::error(ErrorCode::BoundsExceeded, "event stream has too many entries");
    }
  }
  return events;
}

std::vector<std::uint8_t> encode_state_canonical(const DomainState& state) {
  Encoder encoder;
  encoder.u32(1);  // canonical state scheme
  encoder.id(state.domain);
  encoder.counter(state.revision);
  encoder.generation(state.epoch);
  encoder.boolean(state.epoch_established);
  encoder.boolean(state.registered);
  encoder.tick(state.registered_at);
  encoder.generation(state.evidence_generation);
  encoder.digest(state.evidence_digest);
  encoder.id(state.active_plan);
  encoder.id(state.active_attempt);
  encoder.counter(state.last_commit);
  encoder.tick(state.last_tick);
  encoder.boolean(state.recovered);
  encoder.boolean(state.has_topology);
  encoder.boolean(state.has_capacity);
  encoder.boolean(state.has_policy);
  encoder.boolean(state.has_obligations);
  encoder.boolean(state.has_authority);
  const std::vector<std::uint8_t> stream = encode_snapshot_events(state);
  encoder.blob(stream);
  // Health streaks are derived, non-durable observations that must be re-earned
  // after a restart, so they are deliberately not part of the canonical state.
  encoder.count(state.idempotency.size());
  for (const auto& entry : state.idempotency) {
    encoder.id(entry.first);
    encoder.digest(entry.second.fingerprint);
    encoder.u8(static_cast<std::uint8_t>(entry.second.outcome));
    encoder.id(entry.second.command);
  }
  return encoder.take();
}

}  // namespace cooling_failover
