// Cooling Failover - SYNTHETIC facility harness.
//
// Everything in this header is a synthetic stand-in for a real cooling plant.
// It exists so the orchestration semantics can be exercised without a data
// centre. It is NOT hardware validation and must never be described as such.
//
// The production library never depends on this header; only tests, examples,
// the CLI and the benchmark do.
#pragma once

#include "cooling_failover/adapters.hpp"
#include "cooling_failover/export.hpp"
#include "cooling_failover/model.hpp"
#include "cooling_failover/orchestrator.hpp"
#include "cooling_failover/plan.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace cooling_failover {
namespace synthetic {

/// One source group in the synthetic plant.
struct CF_API SyntheticGroup {
  SourceGroupId group{};
  SourceKind kind{SourceKind::Chiller};
  CoolingSourceId source{};
  bool designated_primary{false};
  Watts nominal{};
  std::vector<ReserveBlockId> owns_blocks{};
};

/// One reserve block. A block listed by several groups is shared reserve and is
/// exactly how double counting would arise if the ledger counted group totals.
struct CF_API SyntheticBlock {
  ReserveBlockId block{};
  Watts nominal{};
  std::vector<SourceGroupId> serving_groups{};
};

struct CF_API SyntheticSpec {
  FailoverDomainId domain{};
  std::vector<SyntheticGroup> groups{};
  std::vector<SyntheticBlock> blocks{};
  std::vector<ProtectedObligation> obligations{};
  RedundancyPolicy policy{};
  TopologyGeneration topology_generation{TopologyGeneration::from_value(1)};
  CapacityGeneration capacity_generation{CapacityGeneration::from_value(1)};
};

struct CF_API SyntheticMutation {
  std::map<CoolingSourceId, HealthState> health{};
  std::map<ReserveBlockId, std::pair<AvailabilityState, Watts>> blocks{};
  AuthorityState authority{AuthorityState::Granted};
  bool authority_present{true};
  ControlPlaneEpoch authority_epoch{ControlPlaneEpoch::from_value(1)};
};

/// Deterministic synthetic plant. Every observation it produces is fabricated.
class CF_API SyntheticFacility {
 public:
  explicit SyntheticFacility(SyntheticSpec spec);

  [[nodiscard]] const SyntheticSpec& spec() const noexcept { return spec_; }
  [[nodiscard]] FailoverDomainId domain() const noexcept { return spec_.domain; }

  [[nodiscard]] TopologyProjection topology(EvidenceId evidence, Tick now) const;
  [[nodiscard]] CapacityAvailabilityEvidence capacity(EvidenceId evidence, Tick now) const;
  [[nodiscard]] SourceHealthEvidence health_for(CoolingSourceId source, EvidenceId evidence,
                                                Tick now) const;
  [[nodiscard]] ServiceAuthorityEvidence authority(EvidenceId evidence, Tick now) const;
  [[nodiscard]] ObligationSet obligations(EvidenceId evidence, Tick now) const;

  void set_topology_generation(TopologyGeneration generation) noexcept {
    spec_.topology_generation = generation;
  }
  void set_capacity_generation(CapacityGeneration generation) noexcept {
    spec_.capacity_generation = generation;
  }
  void set_health(CoolingSourceId source, HealthState state);
  void set_block(ReserveBlockId block, AvailabilityState state, Watts available);
  void set_authority(AuthorityState state, ControlPlaneEpoch epoch);

  [[nodiscard]] HealthState health_of(CoolingSourceId source) const;
  [[nodiscard]] AvailabilityState block_state(ReserveBlockId block) const;
  [[nodiscard]] Watts block_capacity(ReserveBlockId block) const;

  /// Marks every source group unavailable except \p survivors.
  void fail_all_except(const std::vector<SourceGroupId>& survivors);

 private:
  SyntheticSpec spec_{};
  SyntheticMutation mutation_{};
  std::map<ReserveBlockId, Watts> block_capacity_{};
};

/// Behaviour of one synthetic control runtime.
struct CF_API SyntheticRuntimeBehaviour {
  bool reachable{true};
  bool acknowledges{true};
  /// Emit real effect observations from the owning runtime after issue.
  bool emits_effects{true};
  /// Emit only acknowledgement-origin observations (never proof of transfer).
  bool acknowledgement_only{false};
  /// Effects that are never reported, which produces a partial transition.
  std::set<EffectKind> suppressed_effects{};
  /// Effect sequences start here and advance by one per observation. They must
  /// exceed the orchestrator's command issue sequences to count as fresh.
  std::uint64_t first_sequence{1000000};
};

/// Synthetic control runtime standing in for Airflow Control / Liquid Cooling.
class CF_API SyntheticControlRuntime : public ControlRuntimePort {
 public:
  explicit SyntheticControlRuntime(OwnerSystem runtime, std::string label);

  [[nodiscard]] const char* name() const noexcept override;
  [[nodiscard]] Result<ControlResponse> issue(const ControlRequest& request) override;
  [[nodiscard]] Result<std::vector<EffectObservationRecord>> poll_effects(
      const ControlRequest& request) override;

  void set_behaviour(const SyntheticRuntimeBehaviour& behaviour) noexcept {
    behaviour_ = behaviour;
    next_sequence_ = behaviour.first_sequence;
  }

  /// Generations the runtime stamps on its observations. They must match the
  /// generations the orchestrator currently holds for the observation to verify.
  void set_generations(TopologyGeneration topology, CapacityGeneration capacity) noexcept {
    topology_generation_ = topology;
    capacity_generation_ = capacity;
  }
  [[nodiscard]] const SyntheticRuntimeBehaviour& behaviour() const noexcept { return behaviour_; }

  /// Number of calls to issue().
  [[nodiscard]] std::uint32_t issue_calls() const noexcept { return issue_calls_; }
  /// Number of distinct commands actually actuated. A replay must not increase
  /// this count.
  [[nodiscard]] std::uint32_t actuations() const noexcept { return actuations_; }
  [[nodiscard]] std::uint32_t poll_calls() const noexcept { return poll_calls_; }
  [[nodiscard]] const std::set<std::uint64_t>& actuated_commands() const noexcept {
    return actuated_commands_;
  }
  void reset_counters();

 private:
  OwnerSystem runtime_{OwnerSystem::AirflowControl};
  std::string label_{};
  SyntheticRuntimeBehaviour behaviour_{};
  std::uint32_t issue_calls_{0};
  std::uint32_t actuations_{0};
  std::uint32_t poll_calls_{0};
  std::uint64_t next_sequence_{1000000};
  TopologyGeneration topology_generation_{TopologyGeneration::from_value(1)};
  CapacityGeneration capacity_generation_{CapacityGeneration::from_value(1)};
  std::set<std::uint64_t> actuated_commands_{};
};

/// Builds the two-group synthetic plant used by the examples and the CLI.
[[nodiscard]] CF_API SyntheticSpec two_group_plant(FailoverDomainId domain);

}  // namespace synthetic
}  // namespace cooling_failover
