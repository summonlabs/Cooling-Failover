// Cooling Failover - runnable end-to-end example.
//
// The plant, its sensors and its control runtimes in this example are
// SYNTHETIC. Nothing here is hardware validation.
#include "cooling_failover/orchestrator.hpp"
#include "cooling_failover/synthetic.hpp"
#include "cooling_failover/version.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace cf = cooling_failover;
using namespace cooling_failover::synthetic;

namespace {

int report(const cf::Status& status) {
  std::cerr << "error: " << status.canonical_line() << "\n";
  return 1;
}

std::string cause_list(const cf::FailoverCandidate& candidate) {
  std::string text;
  for (cf::EligibilityCause cause : candidate.causes) {
    if (!text.empty()) {
      text += ",";
    }
    text += cf::to_string(cause);
  }
  return text;
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path root =
      argc > 1 ? std::filesystem::path(argv[1])
               : std::filesystem::temp_directory_path() / "cooling-failover-example";
  std::filesystem::remove_all(root);

  const cf::FailoverDomainId domain = cf::FailoverDomainId::from_value(0xDCCF0001);
  const cf::ControlPlaneEpoch epoch = cf::ControlPlaneEpoch::from_value(1);

  SyntheticFacility facility(two_group_plant(domain));
  SyntheticControlRuntime airflow(cf::OwnerSystem::AirflowControl, "example-airflow");
  SyntheticControlRuntime liquid(cf::OwnerSystem::LiquidCoolingControl, "example-liquid");

  cf::OrchestratorOptions options;
  options.store.root = root;
  auto opened = cf::CoolingFailoverOrchestrator::open(options, domain);
  if (!opened.has_value()) {
    return report(opened.status());
  }
  std::unique_ptr<cf::CoolingFailoverOrchestrator> orchestrator = std::move(*opened);
  orchestrator->set_control_runtime(cf::OwnerSystem::AirflowControl, &airflow);
  orchestrator->set_control_runtime(cf::OwnerSystem::LiquidCoolingControl, &liquid);

  cf::Tick now = cf::Tick::from_value(0);
  std::uint64_t next_evidence = 1000;
  const auto evidence = [&next_evidence] {
    next_evidence += 1;
    return cf::EvidenceId::from_value(next_evidence);
  };

  std::cout << "cooling-failover " << cf::version_string() << " (" << cf::build_identity() << ")\n";
  std::cout << "store root: " << root.string() << "\n";
  std::cout << "NOTE: the facility, sensors and control runtimes below are SYNTHETIC.\n\n";

  if (auto status = orchestrator->register_domain(now); !status.has_value()) {
    return report(status.status());
  }
  if (auto status = orchestrator->import_topology(facility.topology(evidence(), now));
      !status.has_value()) {
    return report(status.status());
  }
  if (auto status = orchestrator->record_capacity(facility.capacity(evidence(), now));
      !status.has_value()) {
    return report(status.status());
  }
  if (auto status = orchestrator->install_policy(facility.spec().policy); !status.has_value()) {
    return report(status.status());
  }
  if (auto status = orchestrator->install_obligations(facility.obligations(evidence(), now));
      !status.has_value()) {
    return report(status.status());
  }
  if (auto status = orchestrator->record_authority(facility.authority(evidence(), now));
      !status.has_value()) {
    return report(status.status());
  }
  for (const SyntheticGroup& group : facility.spec().groups) {
    if (auto status = orchestrator->record_health(facility.health_for(group.source, evidence(), now));
        !status.has_value()) {
      return report(status.status());
    }
  }
  if (auto status = orchestrator->establish_epoch(epoch, now); !status.has_value()) {
    return report(status.status());
  }

  {
    auto candidates = orchestrator->evaluate_candidates(now);
    if (!candidates.has_value()) {
      return report(candidates.status());
    }
    std::cout << "candidate arrangements (generation-bound):\n";
    for (const cf::FailoverCandidate& candidate : candidates->candidates) {
      std::cout << "  " << candidate.id.to_string() << " groups=" << candidate.key.groups.size()
                << " usable=" << candidate.usable_capacity.to_string()
                << " state=" << cf::to_string(candidate.state);
      if (!candidate.causes.empty()) {
        std::cout << " causes=" << cause_list(candidate);
      }
      if (candidate.rank != UINT32_MAX) {
        std::cout << " rank=" << candidate.rank;
      }
      std::cout << "\n";
    }
    std::cout << "selected: "
              << (candidates->selected.is_nil() ? std::string("<none>")
                                                : candidates->selected.to_string())
              << "\n\n";
  }

  std::cout << "injecting failure: primary source group becomes unavailable\n";
  now = cf::Tick::from_value(5);
  facility.fail_all_except({cf::SourceGroupId::from_value(2)});
  for (const SyntheticGroup& group : facility.spec().groups) {
    if (auto status = orchestrator->record_health(facility.health_for(group.source, evidence(), now));
        !status.has_value()) {
      return report(status.status());
    }
  }

  cf::PlanRequest request;
  request.request = cf::RequestId::from_value(1);
  request.now = now;
  request.kind = cf::PlanKind::Failover;
  request.incumbent = cf::SourceGroupId::from_value(1);
  request.reason = "primary chiller group unavailable";

  auto plan = orchestrator->create_plan(request);
  if (!plan.has_value()) {
    return report(plan.status());
  }
  std::cout << "plan " << plan->id.to_string() << " target "
            << plan->target.to_string() << " steps " << plan->steps.size() << " commands "
            << plan->commands.size() << "\n";
  std::cout << "  bound topology=" << plan->topology.value() << " capacity=" << plan->capacity.value()
            << " policy=" << plan->policy.value() << " epoch=" << plan->epoch.value() << "\n";

  auto attempt = orchestrator->execute_plan(plan->id, now);
  if (!attempt.has_value()) {
    return report(attempt.status());
  }
  std::cout << "attempt " << attempt->id.to_string() << " state "
            << cf::to_string(attempt->state) << "\n";
  std::cout << "  commands issued=" << attempt->commands_issued
            << " replayed=" << attempt->commands_replayed
            << " refused=" << attempt->commands_refused << "\n";
  std::cout << "  verified effects:";
  for (cf::EffectKind effect : attempt->verified_effects) {
    std::cout << " " << cf::to_string(effect);
  }
  std::cout << "\n  missing effects:";
  for (cf::EffectKind effect : attempt->missing_effects) {
    std::cout << " " << cf::to_string(effect);
  }
  std::cout << "\n\n";

  auto status = orchestrator->status();
  if (!status.has_value()) {
    return report(status.status());
  }
  std::cout << "domain status: revision=" << status->revision.value()
            << " commit=" << status->commit.value() << " plans=" << status->plan_count
            << " attempts=" << status->attempt_count
            << " observations=" << status->observation_count
            << " degraded=" << (status->degraded ? "true" : "false") << "\n";
  std::cout << "state digest: " << status->state_digest.to_hex() << "\n";

  if (auto closed = orchestrator->close(); !closed.has_value()) {
    return report(closed.status());
  }
  std::cout << "\nexample complete; store left at " << root.string() << "\n";
  return 0;
}
