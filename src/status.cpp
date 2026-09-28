#include "cooling_failover/status.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <type_traits>

namespace cooling_failover {

const char* to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok: return "Ok";
    case ErrorCode::InvalidArgument: return "InvalidArgument";
    case ErrorCode::MalformedIdentity: return "MalformedIdentity";
    case ErrorCode::OutOfRange: return "OutOfRange";
    case ErrorCode::BoundsExceeded: return "BoundsExceeded";
    case ErrorCode::DuplicateIdentity: return "DuplicateIdentity";
    case ErrorCode::EmptyRequiredField: return "EmptyRequiredField";
    case ErrorCode::InvalidEnumValue: return "InvalidEnumValue";
    case ErrorCode::InvalidText: return "InvalidText";
    case ErrorCode::InvalidPath: return "InvalidPath";
    case ErrorCode::PathTooLong: return "PathTooLong";
    case ErrorCode::PathAmbiguous: return "PathAmbiguous";
    case ErrorCode::PathReparsePoint: return "PathReparsePoint";
    case ErrorCode::StaleGeneration: return "StaleGeneration";
    case ErrorCode::FutureGeneration: return "FutureGeneration";
    case ErrorCode::SupersededEpoch: return "SupersededEpoch";
    case ErrorCode::EpochMismatch: return "EpochMismatch";
    case ErrorCode::EpochNotEstablished: return "EpochNotEstablished";
    case ErrorCode::StalePlan: return "StalePlan";
    case ErrorCode::PlanFenced: return "PlanFenced";
    case ErrorCode::PlanNotFound: return "PlanNotFound";
    case ErrorCode::PlanAlreadyActive: return "PlanAlreadyActive";
    case ErrorCode::PlanNotActive: return "PlanNotActive";
    case ErrorCode::AuthorityDenied: return "AuthorityDenied";
    case ErrorCode::AuthorityUnknown: return "AuthorityUnknown";
    case ErrorCode::AuthorityMissing: return "AuthorityMissing";
    case ErrorCode::AuthoritySuperseded: return "AuthoritySuperseded";
    case ErrorCode::EvidenceMissing: return "EvidenceMissing";
    case ErrorCode::EvidenceStale: return "EvidenceStale";
    case ErrorCode::EvidenceFuture: return "EvidenceFuture";
    case ErrorCode::EvidenceGenerationMismatch: return "EvidenceGenerationMismatch";
    case ErrorCode::EvidenceIndeterminate: return "EvidenceIndeterminate";
    case ErrorCode::EvidenceUnsupported: return "EvidenceUnsupported";
    case ErrorCode::EvidenceKindMismatch: return "EvidenceKindMismatch";
    case ErrorCode::EvidenceOwnerMismatch: return "EvidenceOwnerMismatch";
    case ErrorCode::EvidenceRecoveredStale: return "EvidenceRecoveredStale";
    case ErrorCode::EvidenceReordered: return "EvidenceReordered";
    case ErrorCode::NoEligibleCandidate: return "NoEligibleCandidate";
    case ErrorCode::SelectionNotDefined: return "SelectionNotDefined";
    case ErrorCode::PolicyNotDefined: return "PolicyNotDefined";
    case ErrorCode::PolicyMismatch: return "PolicyMismatch";
    case ErrorCode::DuplicateReserveUse: return "DuplicateReserveUse";
    case ErrorCode::InsufficientHeadroom: return "InsufficientHeadroom";
    case ErrorCode::InsufficientIndependence: return "InsufficientIndependence";
    case ErrorCode::ObligationUnprotected: return "ObligationUnprotected";
    case ErrorCode::CandidateStale: return "CandidateStale";
    case ErrorCode::TopologyNotImported: return "TopologyNotImported";
    case ErrorCode::AttemptNotFound: return "AttemptNotFound";
    case ErrorCode::AttemptAlreadyTerminal: return "AttemptAlreadyTerminal";
    case ErrorCode::PartialTransition: return "PartialTransition";
    case ErrorCode::EffectNotObserved: return "EffectNotObserved";
    case ErrorCode::AcknowledgementIsNotProof: return "AcknowledgementIsNotProof";
    case ErrorCode::DuplicateCommand: return "DuplicateCommand";
    case ErrorCode::IdempotencyConflict: return "IdempotencyConflict";
    case ErrorCode::CommandRefused: return "CommandRefused";
    case ErrorCode::AdapterUnavailable: return "AdapterUnavailable";
    case ErrorCode::DegradedOperation: return "DegradedOperation";
    case ErrorCode::AttemptInterrupted: return "AttemptInterrupted";
    case ErrorCode::ObservationUnknownCommand: return "ObservationUnknownCommand";
    case ErrorCode::HysteresisNotSatisfied: return "HysteresisNotSatisfied";
    case ErrorCode::RecoveryNotEligible: return "RecoveryNotEligible";
    case ErrorCode::IncumbentHealthy: return "IncumbentHealthy";
    case ErrorCode::OscillationGuard: return "OscillationGuard";
    case ErrorCode::StoreNotFound: return "StoreNotFound";
    case ErrorCode::StoreCorrupt: return "StoreCorrupt";
    case ErrorCode::StoreTruncated: return "StoreTruncated";
    case ErrorCode::StoreVersionUnsupported: return "StoreVersionUnsupported";
    case ErrorCode::StoreLocked: return "StoreLocked";
    case ErrorCode::StoreIoError: return "StoreIoError";
    case ErrorCode::StoreAlreadyOpen: return "StoreAlreadyOpen";
    case ErrorCode::StoreClosed: return "StoreClosed";
    case ErrorCode::StoreTrailingBytes: return "StoreTrailingBytes";
    case ErrorCode::StoreRolledBack: return "StoreRolledBack";
    case ErrorCode::StoreLimitExceeded: return "StoreLimitExceeded";
    case ErrorCode::StoreRecoveredStale: return "StoreRecoveredStale";
    case ErrorCode::StoreReservedFieldSet: return "StoreReservedFieldSet";
    case ErrorCode::Overflow: return "Overflow";
    case ErrorCode::LimitExceeded: return "LimitExceeded";
    case ErrorCode::CapacityExceeded: return "CapacityExceeded";
    case ErrorCode::InternalError: return "InternalError";
    case ErrorCode::NotImplemented: return "NotImplemented";
    case ErrorCode::Unsupported: return "Unsupported";
  }
  return "UnknownErrorCode";
}

ErrorClass classify(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok:
      return ErrorClass::None;

    case ErrorCode::InvalidArgument:
    case ErrorCode::MalformedIdentity:
    case ErrorCode::OutOfRange:
    case ErrorCode::BoundsExceeded:
    case ErrorCode::DuplicateIdentity:
    case ErrorCode::EmptyRequiredField:
    case ErrorCode::InvalidEnumValue:
    case ErrorCode::InvalidText:
    case ErrorCode::InvalidPath:
    case ErrorCode::PathTooLong:
    case ErrorCode::PathAmbiguous:
    case ErrorCode::PathReparsePoint:
    case ErrorCode::EvidenceKindMismatch:
    case ErrorCode::EvidenceOwnerMismatch:
    case ErrorCode::AcknowledgementIsNotProof:
    case ErrorCode::StoreReservedFieldSet:
      return ErrorClass::Invalid;

    case ErrorCode::StaleGeneration:
    case ErrorCode::FutureGeneration:
    case ErrorCode::SupersededEpoch:
    case ErrorCode::EpochMismatch:
    case ErrorCode::StalePlan:
    case ErrorCode::AuthoritySuperseded:
    case ErrorCode::EvidenceStale:
    case ErrorCode::EvidenceFuture:
    case ErrorCode::EvidenceGenerationMismatch:
    case ErrorCode::EvidenceRecoveredStale:
    case ErrorCode::EvidenceReordered:
    case ErrorCode::CandidateStale:
      return ErrorClass::Stale;

    case ErrorCode::AuthorityDenied:
    case ErrorCode::CommandRefused:
      return ErrorClass::Denied;

    case ErrorCode::AuthorityUnknown:
    case ErrorCode::AuthorityMissing:
    case ErrorCode::ObservationUnknownCommand:
    case ErrorCode::EvidenceIndeterminate:
    case ErrorCode::EvidenceUnsupported:
    case ErrorCode::PartialTransition:
    case ErrorCode::EffectNotObserved:
    case ErrorCode::DegradedOperation:
    case ErrorCode::AttemptInterrupted:
      return ErrorClass::Indeterminate;

    case ErrorCode::EpochNotEstablished:
    case ErrorCode::PlanFenced:
    case ErrorCode::PlanAlreadyActive:
    case ErrorCode::PlanNotActive:
    case ErrorCode::NoEligibleCandidate:
    case ErrorCode::SelectionNotDefined:
    case ErrorCode::DuplicateReserveUse:
    case ErrorCode::InsufficientHeadroom:
    case ErrorCode::InsufficientIndependence:
    case ErrorCode::ObligationUnprotected:
    case ErrorCode::HysteresisNotSatisfied:
    case ErrorCode::RecoveryNotEligible:
    case ErrorCode::OscillationGuard:
    case ErrorCode::DuplicateCommand:
    case ErrorCode::IdempotencyConflict:
    case ErrorCode::AttemptAlreadyTerminal:
    case ErrorCode::StoreLocked:
    case ErrorCode::StoreAlreadyOpen:
    case ErrorCode::StoreClosed:
    case ErrorCode::CapacityExceeded:
      return ErrorClass::Conflict;

    case ErrorCode::PolicyNotDefined:
    case ErrorCode::PolicyMismatch:
    case ErrorCode::TopologyNotImported:
    case ErrorCode::PlanNotFound:
    case ErrorCode::AttemptNotFound:
    case ErrorCode::IncumbentHealthy:
    case ErrorCode::EvidenceMissing:
      return ErrorClass::Unavailable;

    case ErrorCode::StoreNotFound:
    case ErrorCode::StoreCorrupt:
    case ErrorCode::StoreTruncated:
    case ErrorCode::StoreIoError:
    case ErrorCode::StoreTrailingBytes:
    case ErrorCode::StoreRolledBack:
    case ErrorCode::StoreLimitExceeded:
    case ErrorCode::AdapterUnavailable:
      return ErrorClass::Unavailable;

    case ErrorCode::StoreVersionUnsupported:
    case ErrorCode::Unsupported:
      return ErrorClass::Unsupported;

    case ErrorCode::Overflow:
    case ErrorCode::LimitExceeded:
    case ErrorCode::InternalError:
    case ErrorCode::NotImplemented:
      return ErrorClass::Internal;
  }
  return ErrorClass::Internal;
}

namespace detail {

std::string to_context_string(std::string_view value) { return std::string(value); }
std::string to_context_string(const std::string& value) { return value; }
std::string to_context_string(const char* value) { return value == nullptr ? std::string() : std::string(value); }
std::string to_context_string(bool value) { return value ? "true" : "false"; }

namespace {
template <class T>
std::string integral_to_string(T value) {
  // std::to_string would be locale independent for integers, but this is a
  // direct, allocation-bounded conversion.
  if (value == 0) {
    return "0";
  }
  const bool negative = value < 0;
  using Unsigned = std::make_unsigned_t<T>;
  Unsigned magnitude = negative ? static_cast<Unsigned>(0) - static_cast<Unsigned>(value)
                                : static_cast<Unsigned>(value);
  char digits[24];
  std::size_t used = 0;
  while (magnitude != 0 && used < sizeof(digits)) {
    digits[used++] = static_cast<char>('0' + static_cast<int>(magnitude % 10));
    magnitude /= 10;
  }
  std::string out;
  out.reserve(used + (negative ? 1U : 0U));
  if (negative) {
    out.push_back('-');
  }
  for (std::size_t i = used; i > 0; --i) {
    out.push_back(digits[i - 1]);
  }
  return out;
}
}  // namespace

std::string to_context_string(std::uint64_t value) { return integral_to_string(value); }
std::string to_context_string(std::int64_t value) { return integral_to_string(value); }
std::string to_context_string(std::uint32_t value) { return integral_to_string(value); }
std::string to_context_string(std::int32_t value) { return integral_to_string(value); }

}  // namespace detail

void Status::set_context(std::string key, std::string value) {
  for (auto& entry : context_) {
    if (entry.first == key) {
      entry.second = std::move(value);
      return;
    }
  }
  context_.emplace_back(std::move(key), std::move(value));
  std::sort(context_.begin(), context_.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
}

std::string Status::to_string() const {
  std::string out = std::string(cooling_failover::to_string(code_));
  if (!message_.empty()) {
    out += ": ";
    out += message_;
  }
  for (const auto& entry : context_) {
    out += " [";
    out += entry.first;
    out += "=";
    out += entry.second;
    out += "]";
  }
  return out;
}

std::string Status::canonical_line() const {
  std::string out = std::string(cooling_failover::to_string(code_));
  out += "(";
  out += std::to_string(static_cast<unsigned>(code_));
  out += ")";
  if (!message_.empty()) {
    out += " ";
    out += message_;
  }
  for (const auto& entry : context_) {
    out += " ";
    out += entry.first;
    out += "=";
    out += entry.second;
  }
  return out;
}

}  // namespace cooling_failover
