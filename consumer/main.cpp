// Independent downstream consumer of the installed Cooling Failover package.
//
// It is built out of tree against the installed package only, uses
// find_package(CoolingFailover CONFIG), links CoolingFailover::cooling_failover
// and drives a real library lifecycle: install generation-bound evidence, plan a
// failover, execute it against a control-runtime port, and verify the observed
// effects.
//
// The facility it drives is SYNTHETIC.
#include "cooling_failover/orchestrator.hpp"
#include "cooling_failover/synthetic.hpp"
#include "cooling_failover/version.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>

namespace cf = cooling_failover;
using namespace cooling_failover::synthetic;

int main(int argc, char** argv) {
  const std::filesystem::path root =
      argc > 1 ? std::filesystem::path(argv[1])
               : std::filesystem::temp_directory_path() / "cooling-failover-consumer";
  std::filesystem::remove_all(root);

  std::cout << "consumer linked against cooling_failover " << cf::version_string() << " ("
            << cf::build_identity() << ")\n";

  const cf::FailoverDomainId domain = cf::FailoverDomainId::from_value(0xC0FFEE01);
  const cf::ControlPlaneEpoch epoch = cf::ControlPlaneEpoch::from_value(1);

  SyntheticFacility facility(two_group_plant(domain));
  SyntheticControlRuntime airflow(cf::OwnerSystem::AirflowControl, "consumer-airflow");
  SyntheticControlRuntime liquid(cf::OwnerSystem::LiquidCoolingControl, "consumer-liquid");

  cf::OrchestratorOptions options;
  options.store.root = root;
  auto opened = cf::CoolingFailoverOrchestrator::open(options, domain);
  if (!opened.has_value()) {
    std::cerr << "open failed: " << opened.status().canonical_line() << "\n";
    return 1;
  }
  std::unique_ptr<cf::CoolingFailoverOrchestrator> orchestrator = std::move(*opened);
  orchestrator->set_control_runtime(cf::OwnerSystem::AirflowControl, &airflow);
  orchestrator->set_control_runtime(cf::OwnerSystem::LiquidCoolingControl, &liquid);

  cf::Tick now = cf::Tick::from_value(0);
  std::uint64_t next = 7000;
  const auto evidence = [&next] { return cf::EvidenceId::from_value(++next); };

  if (!orchestrator->register_domain(now).has_value() ||
      !orchestrator->import_topology(facility.topology(evidence(), now)).has_value() ||
      !orchestrator->record_capacity(facility.capacity(evidence(), now)).has_value() ||
      !orchestrator->install_policy(facility.spec().policy).has_value() ||
      !orchestrator->install_obligations(facility.obligations(evidence(), now)).has_value() ||
      !orchestrator->record_authority(facility.authority(evidence(), now)).has_value()) {
    std::cerr << "evidence installation failed\n";
    return 1;
  }
  for (const SyntheticGroup& group : facility.spec().groups) {
    if (!orchestrator->record_health(facility.health_for(group.source, evidence(), now))
             .has_value()) {
      std::cerr << "health evidence failed\n";
      return 1;
    }
  }
  if (!orchestrator->establish_epoch(epoch, now).has_value()) {
    std::cerr << "epoch establishment failed\n";
    return 1;
  }

  now = cf::Tick::from_value(5);
  facility.fail_all_except({cf::SourceGroupId::from_value(2)});
  for (const SyntheticGroup& group : facility.spec().groups) {
    if (!orchestrator->record_health(facility.health_for(group.source, evidence(), now))
             .has_value()) {
      std::cerr << "health evidence failed\n";
      return 1;
    }
  }

  cf::PlanRequest request;
  request.request = cf::RequestId::from_value(1);
  request.now = now;
  request.kind = cf::PlanKind::Failover;
  request.incumbent = cf::SourceGroupId::from_value(1);
  request.reason = "consumer smoke failover";
  auto plan = orchestrator->create_plan(request);
  if (!plan.has_value()) {
    std::cerr << "plan failed: " << plan.status().canonical_line() << "\n";
    return 1;
  }
  auto attempt = orchestrator->execute_plan(plan->id, now);
  if (!attempt.has_value()) {
    std::cerr << "execute failed: " << attempt.status().canonical_line() << "\n";
    return 1;
  }
  std::cout << "plan " << plan->id.to_string() << " -> attempt " << attempt->id.to_string()
            << " state " << cf::to_string(attempt->state) << "\n";
  if (attempt->state != cf::AttemptState::Verified) {
    std::cerr << "failover did not verify\n";
    return 1;
  }
  auto status = orchestrator->status();
  if (!status.has_value()) {
    return 1;
  }
  std::cout << "commit " << status->commit.value() << " state digest "
            << status->state_digest.to_hex() << "\n";
  std::cout << "consumer OK\n";
  return orchestrator->close().has_value() ? 0 : 1;
}
