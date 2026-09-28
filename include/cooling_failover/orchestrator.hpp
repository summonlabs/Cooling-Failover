// Cooling Failover - the public orchestration API.
#pragma once

#include "cooling_failover/adapters.hpp"
#include "cooling_failover/canonical.hpp"
#include "cooling_failover/eligibility.hpp"
#include "cooling_failover/export.hpp"
#include "cooling_failover/ids.hpp"
#include "cooling_failover/model.hpp"
#include "cooling_failover/plan.hpp"
#include "cooling_failover/state.hpp"
#include "cooling_failover/status.hpp"
#include "cooling_failover/store.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cooling_failover {

/// Routes a runtime identity to the port that owns it.
class CF_API ControlRuntimeRegistry {
 public:
  void set(OwnerSystem runtime, ControlRuntimePort* port) noexcept;
  [[nodiscard]] ControlRuntimePort* find(OwnerSystem runtime) const noexcept;

 private:
  ControlRuntimePort* airflow_{nullptr};
  ControlRuntimePort* liquid_{nullptr};
};

struct CF_API OrchestratorOptions {
  StoreOptions store{};
  std::uint32_t max_retained_observations{kMaxRetainedObservations};
  /// Pull effect observations from the owning runtime after issuing a request.
  bool pull_effects_after_issue{true};
  /// Maximum commands issued by one execute_plan call.
  std::uint32_t max_commands_per_execution{64};
};

struct CF_API PlanRequest {
  RequestId request{};
  Tick now{};
  PlanKind kind{PlanKind::Failover};
  SourceGroupId incumbent{};
  /// Explicit target. Required when the policy is ManualOnly; when the policy is
  /// DeterministicRanked a nil target lets the orchestrator select.
  SourceGroupId target{};
  std::string reason{};
};

/// Recovery / rebalance decision for one domain.
struct CF_API RecoveryDecision {
  bool eligible{false};
  EligibilityCause primary_cause{EligibilityCause::None};
  PlanKind kind{PlanKind::ReturnToPrimary};
  SourceGroupId incumbent{};
  SourceGroupId target{};
  CandidateId selected{};
  std::uint32_t consecutive_healthy{0};
  std::uint64_t dwell_ticks{0};
  std::uint32_t transitions_in_window{0};
  std::string detail{};
};

struct CF_API VerificationSummary {
  AttemptState state{AttemptState::Created};
  std::vector<EffectKind> verified{};
  std::vector<EffectKind> missing{};
  bool complete{false};
};

struct CF_API DomainStatus {
  FailoverDomainId domain{};
  StateRevision revision{};
  CommitSequence commit{};
  ControlPlaneEpoch epoch{};
  bool epoch_established{false};
  bool recovered{false};
  AuthorityState authority{AuthorityState::Unknown};
  TopologyGeneration topology{};
  CapacityGeneration capacity{};
  PolicyGeneration policy{};
  bool has_topology{false};
  bool has_capacity{false};
  bool has_policy{false};
  bool has_obligations{false};
  std::size_t group_count{0};
  std::size_t source_count{0};
  std::size_t block_count{0};
  std::size_t plan_count{0};
  std::size_t attempt_count{0};
  std::size_t observation_count{0};
  PlanId active_plan{};
  AttemptId active_attempt{};
  AttemptState active_attempt_state{AttemptState::Created};
  bool degraded{false};
  Digest state_digest{};
};

class CF_API CoolingFailoverOrchestrator {
 public:
  CoolingFailoverOrchestrator(const CoolingFailoverOrchestrator&) = delete;
  CoolingFailoverOrchestrator& operator=(const CoolingFailoverOrchestrator&) = delete;

  /// Opens (or creates) the durable store for \p domain and recovers state.
  /// Fails with StoreLocked when another process holds writer authority.
  [[nodiscard]] static Result<std::unique_ptr<CoolingFailoverOrchestrator>> open(
      OrchestratorOptions options, FailoverDomainId domain);

  ~CoolingFailoverOrchestrator();

  /// Stops accepting new work, flushes durable state and releases writer
  /// authority. Repeated calls are safe.
  [[nodiscard]] Result<void> close();

  void set_control_runtime(OwnerSystem runtime, ControlRuntimePort* port) noexcept;

  [[nodiscard]] const DomainState& state() const noexcept { return state_; }
  [[nodiscard]] FailoverDomainId domain() const noexcept { return domain_; }
  [[nodiscard]] const RecoveryOutcome& recovery() const noexcept;
  [[nodiscard]] bool closed() const noexcept { return closed_; }

  // ---- imported evidence -------------------------------------------------

  [[nodiscard]] Result<StateRevision> register_domain(Tick now);
  [[nodiscard]] Result<StateRevision> establish_epoch(ControlPlaneEpoch epoch, Tick now);
  [[nodiscard]] Result<StateRevision> import_topology(TopologyProjection projection);
  [[nodiscard]] Result<StateRevision> record_capacity(CapacityAvailabilityEvidence evidence);
  [[nodiscard]] Result<StateRevision> record_health(SourceHealthEvidence evidence);
  [[nodiscard]] Result<StateRevision> record_authority(ServiceAuthorityEvidence evidence);
  [[nodiscard]] Result<StateRevision> install_policy(RedundancyPolicy policy);
  [[nodiscard]] Result<StateRevision> install_obligations(ObligationSet obligations);

  // ---- eligibility and planning ------------------------------------------

  [[nodiscard]] Result<CandidateSet> evaluate_candidates(Tick now) const;
  [[nodiscard]] Result<FailoverPlan> create_plan(const PlanRequest& request);
  [[nodiscard]] Result<FailoverPlan> get_plan(PlanId plan) const;
  [[nodiscard]] Result<FailoverPlan> fence_plan(PlanId plan, FenceReason reason, Tick now);

  // ---- transition ---------------------------------------------------------

  [[nodiscard]] Result<TransitionAttempt> execute_plan(PlanId plan, Tick now);
  [[nodiscard]] Result<TransitionAttempt> submit_observation(EffectObservationRecord observation);
  [[nodiscard]] Result<TransitionAttempt> verify_attempt(AttemptId attempt);
  [[nodiscard]] Result<TransitionAttempt> get_attempt(AttemptId attempt) const;
  [[nodiscard]] Result<VerificationSummary> summarize_attempt(AttemptId attempt) const;

  // ---- recovery -----------------------------------------------------------

  [[nodiscard]] Result<RecoveryDecision> evaluate_recovery(Tick now);

  /// Publishes the current state as a snapshot and starts a fresh write-ahead
  /// log. The commit sequence is unchanged and the store still resolves to it
  /// after a crash in either ordering of the two atomic publications.
  [[nodiscard]] Result<void> checkpoint();

  // ---- inspection ---------------------------------------------------------

  [[nodiscard]] Result<DomainStatus> status() const;
  /// Canonical byte encoding of the durable orchestration state. Derived,
  /// non-durable observations (health streaks) are excluded, so the encoding is
  /// stable across a clean restart.
  [[nodiscard]] std::vector<std::uint8_t> canonical_state() const;
  [[nodiscard]] Digest state_digest() const;

 private:
  CoolingFailoverOrchestrator() = default;

  [[nodiscard]] Result<void> resume(std::unique_ptr<DurableStore> store, OrchestratorOptions options);
  [[nodiscard]] Result<void> commit(RecordType type, std::span<const std::uint8_t> payload);
  [[nodiscard]] Result<void> rebuild_from_store();
  void interrupt_recovered_attempts();

  /// Recomputes the evidence digest consumed by \p plan from current evidence.
  [[nodiscard]] Result<Digest> binding_evidence_digest(const FailoverPlan& plan) const;
  [[nodiscard]] Result<void> assert_plan_live(const FailoverPlan& plan) const;
  [[nodiscard]] Result<void> require_open() const;

  /// Fences every active plan whose bound generations or evidence have moved.
  [[nodiscard]] Result<void> enforce_bindings(Tick now);
  [[nodiscard]] Result<void> fence_plan_internal(PlanId plan, FenceReason reason, Tick now);
  [[nodiscard]] static FenceReason fence_reason_for(const Status& status) noexcept;
  [[nodiscard]] Result<void> update_attempt(TransitionAttempt attempt);
  [[nodiscard]] VerificationSummary recompute_verification(const FailoverPlan& plan,
                                                           TransitionAttempt& attempt) const;

  OrchestratorOptions options_{};
  FailoverDomainId domain_{};
  DomainState state_{};
  std::unique_ptr<DurableStore> store_{};
  ControlRuntimeRegistry runtimes_{};
  bool closed_{false};
};

/// Builds the deterministic candidate sets used by evaluate_candidates. Exposed
/// so tests can compare the production path against a reference model.
[[nodiscard]] CF_API Result<std::vector<CandidateKey>> enumerate_arrangements(
    const DomainState& state);

/// Evaluates one arrangement against the current evidence. Deterministic: the
/// same state, arrangement and tick always produce the same candidate.
[[nodiscard]] CF_API Result<FailoverCandidate> evaluate_arrangement(const DomainState& state,
                                                                    const CandidateKey& key,
                                                                    Tick now);

/// Evidence stamps consumed by an arrangement: topology, capacity, policy,
/// obligations, authority and the health of every source the arrangement or the
/// incumbent depends on.
[[nodiscard]] CF_API Result<Digest> compute_evidence_digest(const DomainState& state,
                                                            const std::vector<SourceGroupId>& key,
                                                            SourceGroupId incumbent);

/// Sorts the eligible candidates by the policy's rank order and assigns ranks.
/// Non-eligible candidates keep rank UINT32_MAX.
[[nodiscard]] CF_API Result<void> rank_candidates(std::vector<FailoverCandidate>& candidates,
                                                  const RedundancyPolicy& policy);

}  // namespace cooling_failover
