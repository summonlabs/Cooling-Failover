// Cooling Failover - reserve accounting, eligibility and candidate ranking.
//
// A structurally redundant path is not automatically operationally eligible.
// Eligibility requires current evidence for the topology generation, required
// capacity/headroom, source health, service authority and policy.
#pragma once

#include "cooling_failover/canonical.hpp"
#include "cooling_failover/export.hpp"
#include "cooling_failover/ids.hpp"
#include "cooling_failover/model.hpp"
#include "cooling_failover/status.hpp"
#include "cooling_failover/units.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cooling_failover {

/// A unit of reserve capacity as seen by one arrangement evaluation.
struct CF_API UsableBlock {
  ReserveBlockId block{};
  Watts available{};
  /// True when the block is physically owned by a group inside the arrangement.
  /// Blocks that merely serve the arrangement are counted too, but only once.
  bool owned_by_arrangement{false};

  friend bool operator==(const UsableBlock&, const UsableBlock&) = default;
};

/// One allocation of a reserve block to a protected obligation.
struct CF_API ReserveAllocation {
  ReserveBlockId block{};
  ObligationId obligation{};
  Watts capacity{};

  friend bool operator==(const ReserveAllocation&, const ReserveAllocation&) = default;
};

/// Allocates reserve blocks to obligations. A block is never allocated twice,
/// which is what makes "the same reserve counted twice" structurally impossible.
///
/// Allocation order is deterministic: blocks are considered in ascending
/// ReserveBlockId order until the obligation's requirement is met.
class CF_API ReserveLedger {
 public:
  ReserveLedger() = default;

  [[nodiscard]] bool is_consumed(ReserveBlockId block) const noexcept;
  [[nodiscard]] Watts consumed_total() const noexcept { return consumed_; }
  [[nodiscard]] const std::vector<ReserveAllocation>& allocations() const noexcept {
    return allocations_;
  }

  /// Total capacity of the supplied blocks that has not yet been consumed.
  [[nodiscard]] Watts available_from(const std::vector<UsableBlock>& usable) const;

  /// Allocates up to \p required watts for \p obligation. Returns the watts
  /// actually allocated, which may be less than requested; the caller decides
  /// whether that is a shortfall. Never allocates a consumed block.
  [[nodiscard]] Result<Watts> allocate(ObligationId obligation, Watts required,
                                       const std::vector<UsableBlock>& usable);

  /// Verifies the structural invariant: no block appears in two allocations and
  /// the running total matches the sum of allocations.
  [[nodiscard]] Result<void> validate() const;

 private:
  std::map<ReserveBlockId, std::pair<ObligationId, Watts>> allocations_by_block_{};
  std::vector<ReserveAllocation> allocations_{};
  Watts consumed_{};
};

/// Outcome of evaluating one arrangement against the current evidence.
enum class EligibilityState : std::uint8_t {
  Eligible = 1,
  Ineligible = 2,
  Indeterminate = 3,  ///< evidence is missing or undecidable; never treated as eligible
};

[[nodiscard]] CF_API const char* to_string(EligibilityState state) noexcept;

/// Why an arrangement is not eligible. Codes are stable and machine-readable.
enum class EligibilityCause : std::uint16_t {
  None = 0,
  TopologyNotImported = 1,
  PolicyMissing = 3,
  ObligationsMissing = 5,
  AuthorityMissing = 6,
  AuthorityDenied = 7,
  AuthorityUnknown = 8,
  AuthoritySuperseded = 9,
  EpochNotEstablished = 10,
  EpochMismatch = 11,
  HealthMissing = 12,
  HealthUnsupported = 13,
  HealthDegraded = 14,
  HealthUnavailable = 15,
  NoUsableReserve = 16,
  CapacityEvidenceMissing = 17,
  CapacityEvidenceStale = 18,
  BlockUnavailable = 19,
  BlockUnknown = 20,
  InsufficientHeadroom = 21,
  InsufficientIndependence = 22,
  ObligationUnprotected = 23,
  EvidenceTooOld = 25,
  GroupNotInTopology = 26,
  GroupEmpty = 27,
  ArrangementTooLarge = 28,
  EmptyArrangement = 29,
  EvidenceFromFuture = 30,
};

[[nodiscard]] CF_API const char* to_string(EligibilityCause cause) noexcept;
/// True when the cause makes an arrangement indeterminate rather than ineligible.
[[nodiscard]] CF_API bool cause_is_indeterminate(EligibilityCause cause) noexcept;

/// Ordered, duplicate-free set of source groups forming one arrangement.
struct CF_API CandidateKey {
  std::vector<SourceGroupId> groups{};

  friend bool operator==(const CandidateKey&, const CandidateKey&) = default;
  friend bool operator<(const CandidateKey& lhs, const CandidateKey& rhs) {
    return lhs.groups < rhs.groups;
  }
};

struct CF_API FailoverCandidate {
  CandidateId id{};
  CandidateKey key{};
  FailoverDomainId domain{};
  ControlPlaneEpoch epoch{};
  TopologyGeneration topology{};
  CapacityGeneration capacity{};
  PolicyGeneration policy{};
  EvidenceSetGeneration evidence_generation{};
  Digest evidence_digest{};
  Watts usable_capacity{};
  Watts surplus{};
  std::uint32_t independent_groups{0};
  EligibilityState state{EligibilityState::Indeterminate};
  std::vector<EligibilityCause> causes{};
  std::vector<ReserveAllocation> allocations{};
  std::uint32_t rank{0};

  [[nodiscard]] bool is_eligible() const noexcept { return state == EligibilityState::Eligible; }
};

/// Deterministic, generation-bound set of candidate arrangements.
struct CF_API CandidateSet {
  FailoverDomainId domain{};
  ControlPlaneEpoch epoch{};
  TopologyGeneration topology{};
  CapacityGeneration capacity{};
  PolicyGeneration policy{};
  EvidenceSetGeneration evidence_generation{};
  Digest evidence_digest{};
  Tick evaluated_at{};
  std::vector<FailoverCandidate> candidates{};  ///< ascending by candidate key
  CandidateId selected{};                       ///< nil when no selection is defined
  SelectionMode selection{SelectionMode::NotDefined};

  [[nodiscard]] const FailoverCandidate* find(const CandidateKey& key) const;
  [[nodiscard]] const FailoverCandidate* find(CandidateId id) const;
  [[nodiscard]] std::size_t eligible_count() const noexcept;
};

/// Derives the deterministic candidate identity from its arrangement.
[[nodiscard]] CF_API CandidateId derive_candidate_id(const CandidateKey& key) noexcept;

}  // namespace cooling_failover
